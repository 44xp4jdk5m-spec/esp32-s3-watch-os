/**
 * @file app_power.c
 * @brief Power management implementation.
 */

#include "app_power.h"
#include "app_ble_ancs.h"
#include "app_config.h"
#include "app_events.h"
#include "battery_history.h"
#include "hw_display.h"
#include "lvgl.h"
#include "hw_touch.h"
#include "hw_init.h"
#include "hw_sleep.h"
#include "wake_on_raise.h"
#include "wake_log.h"
#include "watch_face.h"
#include "wifi_file_server.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

static const char *TAG = "APP_PWR";

/* ── AOD state ──────────────────────────────────────────────────────── */
static bool s_aod_enabled = false;

/* Uptime (us) of the last light-sleep wake, for computing the sleep gap. */
static int64_t s_last_wake_us = 0;

/* BLE is fully suspended while the display is off (radio cut).  Tracked so
 * resume only re-arms advertising if we actually suspended it. */
static bool s_ble_suspended = false;

/* Non-display peripherals (audio codecs / SD card / IMU) currently parked
 * while idle on the watch face (AOD or Low-Power dim).  Mirrors the hw-layer
 * park flag so the per-loop reconcile knows when to (un)park. */
static bool s_periphs_parked = false;

static void app_power_ble_suspend(void)
{
    if (s_ble_suspended) return;
    app_ble_ancs_suspend();
    s_ble_suspended = true;
}

static void app_power_ble_resume(void)
{
    if (!s_ble_suspended) return;
    app_ble_ancs_resume();
    s_ble_suspended = false;
}

/* ── AOD low-power mode (Always-On Display) ─────────────────────────── */
#define AOD_BRIGHTNESS       12            /* max dimmed AOD level (%) */
#define AOD_PIXEL_SHIFT_MS   (60 * 1000)   /* nudge face every 60 s */
static int64_t s_last_aod_shift_ms = 0;

/* forward decl — defined below in the AOD section */
static void app_power_aod_pixel_shift_tick(void);

/* Brightness levels are RELATIVE to the user's configured brightness so a
 * low-brightness setup never gets BRIGHTER when the display auto-dims. */
static uint8_t app_power_dim_level(void)
{
    uint8_t half = (uint8_t)(g_config.brightness / 2);
    return (half < 2) ? 2 : half;
}
static uint8_t app_power_aod_level(void)
{
    uint8_t dim = app_power_dim_level();
    return (dim < AOD_BRIGHTNESS) ? dim : AOD_BRIGHTNESS;
}

/* ── Brightness ─────────────────────────────────────────────────────── */

void app_power_set_brightness_smooth(uint8_t target, int step_ms)
{
    int8_t cur = (int8_t)hw_display_get_brightness();
    int8_t tgt = (int8_t)target;
    if (cur < tgt) {
        for (int i = cur; i <= tgt; i += 2) {
            hw_display_set_brightness((uint8_t)i);
            if (step_ms > 0) vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    } else {
        for (int i = cur; i >= tgt; i -= 2) {
            hw_display_set_brightness((uint8_t)i);
            if (step_ms > 0) vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    }
    g_brightness = target;
}

/* ── Graceful reboot ───────────────────────────────────────────────── */

#define APP_POWER_REBOOT_STACK 4096

static void app_power_reboot_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Graceful reboot requested");

    /* Dim the display first so the panel powers down cleanly — avoids an
     * abnormal strip on the next boot. */
    hw_display_set_brightness(0);
    vTaskDelay(pdMS_TO_TICKS(100));

    app_config_save();

    /* Close services that can be mid-write, to avoid data loss. */
    wifi_file_server_stop();
    app_ble_ancs_stop();

    if (bsp_sdcard) {
        esp_err_t er = bsp_sdcard_unmount();   /* flush + unmount SD */
        ESP_LOGI(TAG, "SD unmount: %s", esp_err_to_name(er));
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
    vTaskDelete(NULL);   /* not reached */
}

void app_power_reboot(void)
{
    if (xTaskCreatePinnedToCore(app_power_reboot_task, "reboot",
                                APP_POWER_REBOOT_STACK, NULL, 5, NULL, 0)
        != pdPASS) {
        ESP_LOGE(TAG, "Cannot create reboot task — restarting directly");
        esp_restart();
    }
}

/* ── Touch ──────────────────────────────────────────────────────────── */

bool app_power_is_touch_pressed(void)
{
    lv_indev_t *indev = hw_touch_get_indev();
    if (!indev) return false;
    return (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED);
}

/* ── Wake ───────────────────────────────────────────────────────────── */

void app_power_wake(void)
{
    /* The wake-on-raise detector is only meant to run while the display is
     * off (or in AOD) — stop it whenever the display comes back on.  Safe to
     * call from the detector task itself (it self-cleans in that case). */
    wake_on_raise_stop();

    /* Reset the idle timer FIRST.  The state changes below (exit_aod, the
     * brightness ramp) happen asynchronously to the main loop's inactivity
     * checker, which would otherwise see a stale g_last_activity_us and
     * immediately re-dim / re-enter AOD while we are still waking up. */
    g_last_activity_us = esp_timer_get_time();

    if (g_aod_active) {
        ESP_LOGI(TAG, "Wake from AOD");
        app_power_exit_aod();
        return;
    }

    if (g_display_off) {
        ESP_LOGI(TAG, "Wake from sleep");
        hw_exit_standby();
        app_power_ble_resume();
        /* Ramp straight from off to the configured brightness — no fixed
         * 80/100 flash for low-brightness users. */
        app_power_set_brightness_smooth(g_config.brightness, 8);
        g_display_off = false;
        g_display_dimmed = false;
    } else if (g_display_dimmed) {
        ESP_LOGI(TAG, "Wake from dim");
        app_power_set_brightness_smooth(g_config.brightness, 8);
        g_display_dimmed = false;
    }
}

void app_power_wake_auto(void)
{
    wake_on_raise_stop();

    if (g_aod_active) {
        ESP_LOGI(TAG, "Auto-wake from AOD");
        app_power_exit_aod();
        /* intentionally NOT resetting g_last_activity_us */
        return;
    }

    if (g_display_off) {
        ESP_LOGI(TAG, "Auto-wake from sleep");
        hw_exit_standby();
        app_power_ble_resume();
        /* Ramp straight from off to the configured brightness — no flash. */
        app_power_set_brightness_smooth(g_config.brightness, 8);
        g_display_off = false;
        g_display_dimmed = false;
        /* intentionally NOT resetting g_last_activity_us */
    } else if (g_display_dimmed) {
        ESP_LOGI(TAG, "Auto-wake from dim");
        app_power_set_brightness_smooth(g_config.brightness, 8);
        g_display_dimmed = false;
        /* intentionally NOT resetting g_last_activity_us */
    }
}

/* ── Idle peripheral parking (AOD / Low-Power Mode) ────────────────── */

void app_power_periph_park_tick(void)
{
    /* Park (power down the audio codecs + SD card, and the IMU unless the
     * raise-to-wake gate is running) whenever the watch is idle on the watch
     * face: display fully off (standby), AOD active, or dimmed while Low
     * Power Mode is on.  Restore the instant the user is active or leaves
     * the watch face — the SD/audio/IMU then power back on automatically.
     *
     * Display-off standby parks inside hw_enter_standby() itself, so here we
     * only reconcile the still-screen-on idle states.  hw_park/unpark are
     * idempotent, so overlapping with standby is harmless. */
    bool should_park =
        g_display_off ||
        g_aod_active ||
        (g_config.low_power_mode &&
         g_current_screen == APP_SCREEN_WATCH_FACE &&
         g_display_dimmed);

    if (should_park && !s_periphs_parked) {
        bool keep_imu = hw_sleep_get_keep_imu() || wake_on_raise_is_running();
        if (hw_park_peripherals(keep_imu) == ESP_OK) {
            s_periphs_parked = true;
        }
    } else if (!should_park && s_periphs_parked) {
        hw_unpark_peripherals();
        s_periphs_parked = false;
    }
}

void app_power_periphs_restore(void)
{
    if (!s_periphs_parked) return;
    hw_unpark_peripherals();
    s_periphs_parked = false;
}

bool app_power_periphs_parked(void)
{
    return s_periphs_parked;
}

/* ── Low Power Mode (shared by Settings + control center) ──────────── */

/* Dim timeout value to restore when Low Power Mode is turned off. */
static uint16_t s_lp_saved_dim_timeout = 0;

void app_power_set_low_power(bool enable)
{
    g_config.low_power_mode = enable;

    if (enable) {
        /* Remember the pre-Low-Power dim timeout once, to restore on off. */
        if (s_lp_saved_dim_timeout == 0) {
            s_lp_saved_dim_timeout = g_config.dim_timeout_sec;
        }
        /* Kill running animations + cap render to ~15 FPS so the UI is
         * calmer and renders at far less CPU cost. */
        lv_anim_delete_all();
        lv_obj_set_style_anim_duration(lv_screen_active(), 0, 0);
        hw_display_set_refr_period_ms(66);   /* 1000 / 66 ≈ 15 FPS */
        g_config.dim_timeout_sec = 5;
        app_config_save();
        uint8_t dim_bright = (uint8_t)(g_config.brightness * 80 / 100);
        if (dim_bright < 10) dim_bright = 10;
        app_power_set_brightness_smooth(dim_bright, 4);
    } else {
        lv_obj_set_style_anim_duration(lv_screen_active(), 280, 0);
        hw_display_set_refr_period_ms(0);    /* restore default (~66 FPS) */
        if (s_lp_saved_dim_timeout) {
            g_config.dim_timeout_sec = s_lp_saved_dim_timeout;
            s_lp_saved_dim_timeout = 0;
        }
        app_config_save();
        app_power_set_brightness_smooth(g_config.brightness, 4);
    }

    ESP_LOGI(TAG, "Low Power Mode: %s", enable ? "ON" : "OFF");
}

/* ── Light sleep ────────────────────────────────────────────────────── */

/* Log one light-sleep wake to the real-time log AND the persistent wake log
 * (RTC memory + SPIFFS copy) so the history is readable even when the serial
 * console dropped during light sleep.  Returns this wake's sequence number. */
static uint32_t app_power_log_wake(const char *src, uint8_t cause,
                                   uint8_t decision, uint8_t cnn_ran,
                                   uint8_t cnn_raise, uint16_t cnn_prob_q8,
                                   uint32_t slept_ms, uint32_t ulp_samples)
{
    uint32_t seq = wake_log_add(cause, decision, cnn_ran, cnn_raise,
                                cnn_prob_q8, ulp_samples);
    if (cnn_ran) {
        ESP_LOGI(TAG, "wake #%" PRIu32 " %s (slept %" PRIu32 "ms, imu=%" PRIu32
                      ") CNN=%.3f → %s",
                 seq, src, slept_ms, ulp_samples, (double)(cnn_prob_q8 / 256.0f),
                 (decision == WAKE_LOG_DEC_WAKE) ? "WAKE" : "re-sleep");
    } else {
        ESP_LOGI(TAG, "wake #%" PRIu32 " %s (slept %" PRIu32 "ms) → %s",
                 seq, src, slept_ms,
                 (decision == WAKE_LOG_DEC_WAKE) ? "WAKE" : "re-sleep");
    }
    return seq;
}

static void app_power_light_sleep_idle(void)
{
    while (g_display_off) {
        /* ULP sleep sampler: load + run it right before sleeping.  No main-CPU
         * I2C happens between ulp_run and esp_light_sleep_start, so the ULP
         * has sole use of the bus (GPIO14/15) while it bit-bangs. */
        if (wake_on_raise_ulp_is_armed() && !wake_on_raise_ulp_is_running()) {
            if (wake_on_raise_ulp_run() != ESP_OK) {
                ESP_LOGE(TAG, "ULP sampler failed to start — staying awake (display off)");
                break;   /* fall back to CPU polling this cycle */
            }
        }

        /* Arm a one-shot housekeeping wake for the next local hour boundary
         * so a battery sample is recorded every hour even while the display
         * is off.  The delay is recomputed to the real :00 mark before every
         * sleep — never a constant periodic wake. */
        hw_sleep_set_housekeep_period_us(battery_history_next_wake_us());

        esp_sleep_wakeup_cause_t cause = hw_sleep_light_enter(); /* blocks */

        /* Any wake: halt the ULP + release the I2C bus FIRST, before the main
         * loop touches the bus again. */
        if (wake_on_raise_ulp_is_running()) {
            wake_on_raise_ulp_halt();
        }

        /* Sleep-session timing + ULP sample count (for the wake log). */
        int64_t now_us = esp_timer_get_time();
        uint32_t slept_ms = (s_last_wake_us > 0)
                            ? (uint32_t)((now_us - s_last_wake_us) / 1000) : 0;
        s_last_wake_us = now_us;
        uint32_t ulp_samples = wake_on_raise_ulp_sample_count();

        if (cause == ESP_SLEEP_WAKEUP_TIMER) {
            /* Housekeeping wake at the top of the hour: poll the PMIC (VBUS
             * / charging) and record the battery sample the display-off main
             * loop could not, then go straight back to sleep.  (No CNN — this
             * path was dead before the hw_sleep wake-cause fix, so timer
             * wakes used to run the ESP-DL model every 15 s.) */
            app_power_log_wake("TIMER/housekeep", WAKE_LOG_CAUSE_TIMER,
                               WAKE_LOG_DEC_RE_SLEEP, 0, 0, 0,
                               slept_ms, ulp_samples);
            hw_poll_events();
            battery_history_record();
            continue;
        }

        if (cause == ESP_SLEEP_WAKEUP_ULP && wake_on_raise_ulp_is_armed()) {
            /* ULP wake: an IMU raise-gate wake.  Wake the display for a
             * confirmed raise; otherwise re-arm the ULP and go straight back
             * to sleep.  app_power_wake() is called HERE (not by the main
             * loop) so a raise wake is never lost to the immediate re-sleep
             * path. */
            bool  wake = false;
            float prob = 0.0f;
            if (wake_on_raise_ulp_handle_wake(&wake, &prob) == ESP_OK) {
                app_power_log_wake("ULP-gate", WAKE_LOG_CAUSE_ULP,
                                   wake ? WAKE_LOG_DEC_WAKE : WAKE_LOG_DEC_RE_SLEEP,
                                   1, wake, (uint16_t)(prob * 256.0f),
                                   slept_ms, ulp_samples);
                if (wake) {
                    app_power_wake();   /* confirmed raise → wake the display */
                    wake_log_persist();
                    break;
                }
                continue;    /* not a raise → re-arm ULP + back to light sleep */
            }
            app_power_log_wake("ULP-gate(!ring)", WAKE_LOG_CAUSE_ULP,
                               WAKE_LOG_DEC_RE_SLEEP, 0, 0, 0,
                               slept_ms, ulp_samples);
            continue;
        }

        /* GPIO wake: BOOT (GPIO0) / PWR (GPIO10) are explicit buttons — they
         * always wake the display.  GPIO38 is the touch INT. */
        bool is_button = (gpio_get_level(HW_SLEEP_WAKE_BOOT_GPIO) == 0) ||
                         (gpio_get_level(HW_SLEEP_WAKE_PWR_GPIO) == 1);
        if (is_button || !wake_on_raise_is_enabled()) {
            app_power_log_wake(is_button ? "GPIO/button" : "GPIO/touch",
                               WAKE_LOG_CAUSE_GPIO, WAKE_LOG_DEC_WAKE,
                               0, 0, 0, slept_ms, ulp_samples);
            app_power_wake();   /* button, or plain touch wake (WOR off) */
            wake_log_persist();
            break;
        }

        /* Wake-on-raise ON + GPIO38 (touch INT): a REAL finger on the screen
         * must still wake the watch — read the FT3168 directly.  Only a
         * phantom INT during motion (no touch data) is routed through the CNN
         * so the screen doesn't turn on for a mere movement. */
        if (hw_touch_is_touched()) {
            app_power_log_wake("GPIO/touch", WAKE_LOG_CAUSE_GPIO,
                               WAKE_LOG_DEC_WAKE, 0, 0, 0,
                               slept_ms, ulp_samples);
            app_power_wake();   /* real touch → wake */
            wake_log_persist();
            break;
        }

        /* Phantom touch-INT during motion → let the CNN decide (like a gate
         * wake) — screen only turns on for a confirmed raise. */
        bool  wake2 = false;
        float prob2 = 0.0f;
        if (wake_on_raise_ulp_handle_wake(&wake2, &prob2) == ESP_OK) {
            app_power_log_wake("GPIO/motion", WAKE_LOG_CAUSE_GPIO,
                               wake2 ? WAKE_LOG_DEC_WAKE : WAKE_LOG_DEC_RE_SLEEP,
                               1, wake2, (uint16_t)(prob2 * 256.0f),
                               slept_ms, ulp_samples);
            if (wake2) {
                app_power_wake();   /* confirmed raise → wake the display */
                wake_log_persist();
                break;
            }
            continue;    /* not a raise → re-arm ULP + back to light sleep */
        }
        app_power_log_wake("GPIO/motion(!ring)", WAKE_LOG_CAUSE_GPIO,
                           WAKE_LOG_DEC_RE_SLEEP, 0, 0, 0,
                           slept_ms, ulp_samples);
        continue;
    }
}

/* ── Inactivity check ───────────────────────────────────────────────── */

void app_power_check_inactivity(void)
{
    bool touched = app_power_is_touch_pressed();

    /* Touch always counts as activity and wakes from any low-power state
     * (dim, off, or AOD). */
    if (touched) {
        g_last_activity_us = esp_timer_get_time();
        if (g_aod_active) {
            ESP_LOGI(TAG, "Touch → exit AOD");
            app_power_exit_aod();
            g_last_activity_us = esp_timer_get_time();
        } else if (g_display_off) {
            ESP_LOGI(TAG, "Touch → wake display");
            wake_on_raise_stop();   /* detector only runs while display is off */
            hw_exit_standby();
            app_power_ble_resume();
            /* Ramp straight from off to the configured brightness — no flash. */
            app_power_set_brightness_smooth(g_config.brightness, 8);
            g_display_off = false;
            g_display_dimmed = false;
        } else if (g_display_dimmed) {
            ESP_LOGI(TAG, "Touch → restore brightness");
            app_power_set_brightness_smooth(g_config.brightness, 8);
            g_display_dimmed = false;
        }
        return;
    }

    /* ── AOD mode: keep the screen ON, just dim + 1Hz idle + pixel shift ── */
    if (g_aod_active) {
        app_power_aod_pixel_shift_tick();
        return;
    }

    if (g_display_off) {
        /* Light-sleep re-entry.  With light sleep enabled (and no CPU-based
         * wake-on-raise detector running — that keeps the CPU + IMU on), block
         * in light sleep until a real wake source (touch / GPIO0 / GPIO10 /
         * ULP gate) fires.  In ULP wake-on-raise mode the ULP sampler runs
         * during sleep and app_power_light_sleep_idle() handles its wakes.
         * Timer wakes do housekeeping inside the loop and return to sleep.
         * The touch check above and the button g_pending_wake handling in the
         * main loop run BEFORE this point, so reaching here with the display
         * still off means no real interaction is pending — safe to sleep
         * again (this also absorbs spurious GPIO wakes). */
        if (g_config.light_sleep_enabled && !wake_on_raise_is_running()) {
            app_power_light_sleep_idle();
        }
        return;
    }

    /* AOD is a watch-face feature: with AOD ON on any other screen keep the
     * display fully on — never dim, never turn off. */
    if (s_aod_enabled && g_current_screen != APP_SCREEN_WATCH_FACE) {
        if (g_display_dimmed) {
            app_power_set_brightness_smooth(g_config.brightness, 8);
            g_display_dimmed = false;
        }
        return;
    }

    int64_t idle_ms = (esp_timer_get_time() - g_last_activity_us) / 1000;
    uint32_t dim_timeout_ms = (uint32_t)g_config.dim_timeout_sec * 1000;
    /* Half-brightness hold time: half of dim timeout, max 10 sec */
    uint32_t hold_ms = (dim_timeout_ms / 2);
    if (hold_ms > 10000) hold_ms = 10000;

    if (!g_display_dimmed && idle_ms > (int64_t)dim_timeout_ms) {
        ESP_LOGI(TAG, "%" PRIu32 "s idle → dim to %u%%",
                 g_config.dim_timeout_sec, app_power_dim_level());
        app_power_set_brightness_smooth(app_power_dim_level(), 10);
        g_display_dimmed = true;
    }

    if (g_display_dimmed && !g_display_off && idle_ms > (int64_t)(dim_timeout_ms + hold_ms)) {
        if (s_aod_enabled && g_current_screen == APP_SCREEN_WATCH_FACE) {
            /* AOD: do NOT turn off — dim further into the 1Hz AOD mode.
             * Only dim-to-AOD while on the watch face; other screens keep
             * the normal dim → off behaviour. */
            app_power_enter_aod();
        } else {
            ESP_LOGI(TAG, "%" PRIu32 "s+%" PRIu32 "s idle → display off",
                     g_config.dim_timeout_sec, hold_ms / 1000);
            g_display_off = true;
            g_display_dimmed = false;
            /* Stop LVGL flushing BEFORE touching the SPI panel — a concurrent
             * frame flush deadlocks the SPI bus → task-watchdog freeze. */
            hw_display_pause_rendering();
            app_power_set_brightness_smooth(0, 10);
            hw_display_backlight_off();

            /* Cut the BLE radio while the display is off — no advertising or
             * connection RF events keep the baseband waking during light
             * sleep (the single biggest idle drain on this watch). */
            app_power_ble_suspend();

            if (g_config.light_sleep_enabled) {
                /* ULP sleep sampler: always polls the FT3168 touch controller
                 * during light sleep (its INT line is unreliable on this
                 * board), and additionally does the IMU raise gate when
                 * wake-on-raise is enabled.  hw_enter_standby() keeps the IMU
                 * on only when the raise gate needs it (keep_imu flag set by
                 * ulp_prepare).  The light sleep itself happens in the
                 * g_display_off branch (app_power_light_sleep_idle). */
                if (wake_on_raise_ulp_prepare() == ESP_OK) {
                    hw_enter_standby();
                    ESP_LOGI(TAG, "ULP sleep sampler armed — entering light sleep");
                } else {
                    hw_enter_standby();
                }
            } else if (wake_on_raise_is_enabled() && wake_on_raise_start() == ESP_OK) {
                /* fallback (light sleep off): keep the CPU + IMU running with
                 * the display off so the ESP-DL model can detect a "raise". */
                ESP_LOGI(TAG, "Wake-on-raise active — CPU detector running (display off)");
            } else {
                /* Standby: power down display/touch/imu/audio and put the PMIC
                 * to sleep.  hw_enter_standby() returns right away — the CPU
                 * keeps running, the display stays off, and a touch / GPIO0 /
                 * GPIO10 press wakes it via g_pending_wake.  (When light sleep
                 * is enabled, the g_display_off branch blocks in light sleep.) */
                hw_enter_standby();
            }
        }
    }
}

/* ── AOD (Always-On Display) ────────────────────────────────────────── */

void app_power_set_aod_enabled(bool enable)
{
    s_aod_enabled = enable;
    ESP_LOGI(TAG, "AOD: %s", enable ? "ON (dim → 1Hz AOD after timeout)"
                                    : "OFF (normal timeout)");

    /* Turning AOD OFF while already in AOD mode → restore normal display. */
    if (!enable && g_aod_active) {
        app_power_exit_aod();
        return;
    }

    /* Turning AOD ON: if the display is dimmed/off, wake it to full — the
     * inactivity timer will then dim it back into AOD mode after the timeout. */
    if (enable && (g_display_dimmed || g_display_off)) {
        ESP_LOGI(TAG, "AOD on → waking display");
        wake_on_raise_stop();   /* detector only runs while display is off */
        if (g_display_off) {
            hw_exit_standby();
            app_power_ble_resume();
            app_power_set_brightness_smooth(g_config.brightness, 8);
        }
        g_display_off = false;
        g_display_dimmed = false;
        g_last_activity_us = esp_timer_get_time();
    }
}

bool app_power_get_aod_enabled(void)
{
    return s_aod_enabled;
}

/* ── AOD low-power mode ─────────────────────────────────────────────── */

void app_power_enter_aod(void)
{
    if (g_aod_active) return;
    g_aod_active = true;
    g_display_dimmed = false;

    ESP_LOGI(TAG, "Entering AOD mode — dim %u%%, panel ~1Hz idle + pixel shift",
             app_power_aod_level());

    /* Dim further for AOD (screen stays on, just darker, and never brighter
     * than the dimmed level for low-brightness users). */
    app_power_set_brightness_smooth(app_power_aod_level(), 10);

    /* MIPI DCS Idle Mode ON (0x39): the SH8601 self-refreshes the last frame
     * at ~1 Hz instead of full rate → big AMOLED power saving + less burn-in. */
    hw_display_set_idle(true);

    s_last_aod_shift_ms = esp_timer_get_time() / 1000;

    /* Optional: a wrist-raise also exits AOD (if the user enabled it). */
    if (wake_on_raise_is_enabled() && wake_on_raise_start() == ESP_OK) {
        ESP_LOGI(TAG, "AOD: wake-on-raise detector running");
    }
}

void app_power_exit_aod(void)
{
    if (!g_aod_active) return;
    g_aod_active = false;

    ESP_LOGI(TAG, "Exiting AOD mode — full refresh, brightness %d%%",
             g_config.brightness);

    wake_on_raise_stop();           /* detector only runs while in AOD */
    hw_display_set_idle(false);     /* MIPI DCS Idle Mode OFF (0x38) */
    app_power_set_brightness_smooth(g_config.brightness, 8);
    g_display_dimmed = false;
}

/* Periodic burn-in prevention: nudge the watch face a few pixels. */
static void app_power_aod_pixel_shift_tick(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_aod_shift_ms < AOD_PIXEL_SHIFT_MS) return;
    s_last_aod_shift_ms = now_ms;

    /* Cycle a 2×2 grid so no OLED subpixel stays lit for long. */
    static const int8_t pattern[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
    static uint8_t idx = 0;
    idx = (uint8_t)((idx + 1) & 3);

    if (!hw_display_lock(100)) return;
    watch_face_aod_pixel_shift(pattern[idx][0], pattern[idx][1]);
    hw_display_unlock();
}

bool app_power_get_aod_active(void)
{
    return g_aod_active;
}
