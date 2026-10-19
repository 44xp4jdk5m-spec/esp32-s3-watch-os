/**
 * @file hw_init.c
 * @brief Master hardware initializer — init all, manage power states.
 */

#include "hw_init.h"
#include "hw_config.h"
#include "hw_i2c.h"
#include "hw_axp2101.h"
#include "hw_display.h"
#include "hw_touch.h"
#include "hw_audio.h"
#include "hw_imu.h"
#include "hw_rtc.h"
#include "hw_button.h"
#include "hw_sleep.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HW_INIT";
static hw_power_mode_t s_mode = HW_POWER_MODE_ACTIVE;

/* Non-display peripherals (audio codecs / SD card / IMU) currently parked
 * (powered down).  Tracked so park/unpark are idempotent — used both by
 * full display-off standby and by the AOD / Low-Power idle parking path. */
static bool s_periphs_parked = false;

/* ── Init all ──────────────────────────────────────────────────────────── */

esp_err_t hw_init_all(hw_init_result_t *res)
{
    hw_init_result_t r = {0};
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Hardware Initialization Begin");
    ESP_LOGI(TAG, "========================================");

    /* 1. NVS (needed for WiFi/BT, safe to init early) */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 1b. TCP/IP stack + default event loop — official ESP-IDF pattern.
     *      These must be initialised ONCE globally and NEVER destroyed.
     *      WiFi/SNTP/any network component depends on them. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "netif + event loop initialised");

    /* 2. I2C bus — critical, everything depends on it */
    r.i2c_ok = (hw_i2c_init() == ESP_OK);
    if (!r.i2c_ok) {
        ESP_LOGE(TAG, "FATAL: I2C init failed — cannot continue");
        if (res) *res = r;
        return ESP_FAIL;
    }

    /* 2b. Buttons — MUST init BEFORE AXP2101!
     *      GPIO10 is connected to AXP2101's PEK pin. If AXP2101 starts
     *      monitoring PEK while GPIO10 still has ROM bootloader pull-ups,
     *      the PMIC sees a phantom HIGH→LOW→HIGH transition and powers
     *      back on after a 6 s hardware shutdown. Configuring GPIO10 as
     *      floating input FIRST prevents this. */
    r.button_ok = (hw_button_init() == ESP_OK);
    if (!r.button_ok) ESP_LOGW(TAG, "Buttons not available");

    /* 3. AXP2101 PMIC — critical, must have power */
    r.axp2101_ok = (hw_axp2101_init() == ESP_OK);
    if (!r.axp2101_ok) {
        ESP_LOGE(TAG, "FATAL: AXP2101 init failed — cannot continue");
        if (res) *res = r;
        return ESP_FAIL;
    }

    /* Allow power rails to stabilize before display init.
     * When VBUS is present at boot, the PMIC switches power paths
     * and its DC-DC converters need settling time. Without this,
     * the SH8601 QSPI init commands can be corrupted by power noise,
     * causing horizontal blocks on screen. */
    bool vbus_present = false;
    hw_axp2101_is_vbus_present(&vbus_present);
    if (vbus_present) {
        ESP_LOGI(TAG, "VBUS present — waiting 300ms for rails to stabilize...");
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    /* 4. Display + touch — critical for UI */
    r.display_ok = (hw_display_init() == ESP_OK);
    r.touch_ok   = (hw_touch_init() == ESP_OK);
    if (!r.display_ok) {
        ESP_LOGE(TAG, "FATAL: Display init failed");
        if (res) *res = r;
        return ESP_FAIL;
    }

    /* 5. Audio — non-critical, continue if fails */
    HW_CHECK_CONTINUE(hw_audio_init(), TAG, "Audio init failed");
    r.audio_ok = true; /* approximate — actual status tracked internally */

    /* 6. IMU — non-critical */
    r.imu_ok = (hw_imu_init() == ESP_OK);
    if (!r.imu_ok) ESP_LOGW(TAG, "IMU not available");

    /* 7. RTC — non-critical */
    r.rtc_ok = (hw_rtc_init() == ESP_OK);
    if (!r.rtc_ok) ESP_LOGW(TAG, "RTC not available");

    /* 8. Buttons already initialized at step 2b (before AXP2101) */

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Hardware Init Complete");
    ESP_LOGI(TAG, "  I2C=%s  PMIC=%s  Display=%s  Touch=%s",
             r.i2c_ok ? "OK" : "FAIL",
             r.axp2101_ok ? "OK" : "FAIL",
             r.display_ok ? "OK" : "FAIL",
             r.touch_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  Audio=%s  IMU=%s  RTC=%s  Btn=%s",
             r.audio_ok ? "OK" : "FAIL",
             r.imu_ok ? "OK" : "FAIL",
             r.rtc_ok ? "OK" : "FAIL",
             r.button_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "========================================");

    s_mode = HW_POWER_MODE_ACTIVE;

    if (res) *res = r;
    return ESP_OK;
}

/* ── Peripheral parking (display stays on) ────────────────────────────
 * Park only the peripherals that AOD / Low-Power idle and full display-off
 * standby have in common: audio codecs (ES8311 + ES7210), the SD card, and
 * (unless a gesture gate needs it) the IMU.  The display, touch and PMIC
 * are intentionally left untouched here — those belong to standby. */

esp_err_t hw_park_peripherals(bool keep_imu)
{
    if (s_periphs_parked) return ESP_OK;

    /* IMU: disable — UNLESS the raise-to-wake detector / ULP sampler needs
     * it (keep_imu set when the gesture gate is armed). */
    if (!keep_imu) {
        hw_imu_sleep();
    }

    /* Audio: mute + power down the codecs (ES8311 + ES7210). */
    hw_audio_mute(true);
    hw_audio_sleep();

    /* SD card: power it down (unmount + stop the SDMMC clock) when on
     * battery.  Kept mounted while USB is plugged in so the USB Mass-Storage
     * class (SD over USB) keeps working. */
    bool vbus_present = false;
    if (hw_axp2101_is_vbus_present(&vbus_present) == ESP_OK && !vbus_present) {
        if (bsp_sdcard_power_down() != ESP_OK) {
            ESP_LOGW(TAG, "SD card power-down failed");
        }
    }

    s_periphs_parked = true;
    ESP_LOGI(TAG, "Peripherals parked (audio + SD down%s)",
             keep_imu ? ", IMU kept (gesture)" : ", IMU off");
    return ESP_OK;
}

esp_err_t hw_unpark_peripherals(void)
{
    if (!s_periphs_parked) return ESP_OK;

    hw_imu_wake();
    hw_audio_mute(false);
    hw_audio_wake();
    if (bsp_sdcard_power_up() != ESP_OK) {
        ESP_LOGW(TAG, "SD card re-mount failed after wake");
    }

    s_periphs_parked = false;
    ESP_LOGI(TAG, "Peripherals unparked (IMU, audio + SD back on)");
    return ESP_OK;
}

/* ── Standby / Low-power ───────────────────────────────────────────────── */

esp_err_t hw_enter_standby(void)
{
    if (s_mode == HW_POWER_MODE_STANDBY) return ESP_OK;

    ESP_LOGI(TAG, ">>> Entering STANDBY mode <<<");

    /* Pause LVGL FIRST so it never flushes to the panel while we power it
     * down — a concurrent SPI flush races the brightness/standby commands on
     * the same bus and deadlocks (task-watchdog freeze). */
    hw_display_pause_rendering();

    /* Display: off */
    hw_display_set_brightness(0);
    hw_display_backlight_off();
    hw_display_sleep();

    /* Touch: monitor mode */
    hw_touch_sleep();

    /* Park the non-display peripherals (IMU-if-not-needed, audio, SD). */
    hw_park_peripherals(hw_sleep_get_keep_imu());

    /* PMIC: sleep mode */
    hw_axp2101_enter_sleep();

    s_mode = HW_POWER_MODE_STANDBY;
    ESP_LOGI(TAG, "Standby mode active — display off, IMU off, audio off, PMIC sleep");

    /* ── No light sleep ──────────────────────────────────────────────
     * Light sleep was REMOVED on purpose: after waking from
     * esp_light_sleep_start() the ESP32-S3 (with PSRAM + QSPI flash)
     * can run slowly or glitch while MSPI/cache are re-initialised,
     * which showed up as a slow / frozen UI after sleep.
     *
     * The CPU keeps running here — the main loop simply keeps polling
     * with the display off.  GPIO0 / GPIO10 polls or the touch IRQ
     * wake the screen via g_pending_wake.  No CPU sleep at all. */
    return ESP_OK;
}

esp_err_t hw_exit_standby(void)
{
    /* Always resume LVGL rendering FIRST — the display may have been paused
     * without a full standby cycle (e.g. wake-on-raise keeps the CPU running
     * with the display off, so hw_enter_standby() was never called and s_mode
     * is still ACTIVE).  Resuming must NOT be gated on s_mode, or the UI
     * stays frozen after wake.  No-op if LVGL was never paused. */
    hw_display_resume_rendering();

    if (s_mode == HW_POWER_MODE_ACTIVE) return ESP_OK;

    ESP_LOGI(TAG, "<<< Exiting STANDBY mode >>>");

    /* PMIC: wake */
    hw_axp2101_exit_sleep();

    /* Display: on — brightness is restored by the caller (app_power ramps to
     * the user's configured level).  Never flash a fixed 80/100 here, or a
     * low-brightness user sees a bright blink on every wake. */
    hw_display_wake();

    /* Touch: wake + re-init.  The FT3168 is re-initialised after light sleep
     * so the touch controller is back in a known state.
     *
     * CRITICAL: hw_touch_reinit() hardware-resets the FT3168 (GPIO9) and
     * rewrites its registers.  The LVGL task reads the touch controller over
     * I2C from its own thread — if it is mid-read while we reset the chip,
     * the I2C transaction is interrupted and the LVGL touch input wedges
     * (screen shows UI fine but touches do nothing).  Hold the LVGL lock
     * around the re-init so the indev cannot read while the chip is being
     * reset. */
    hw_touch_wake();
    if (!hw_display_lock(2000)) {
        ESP_LOGW(TAG, "LVGL lock timeout during touch re-init — racing LVGL read");
    }
    /* Register re-write + bus reset, THEN a full rebuild of the touch chain
     * (fresh panel-io -> esp_lcd_touch -> LVGL indev).  Rebuilding the LVGL
     * indev is what actually fixes "I2C alive but UI touch dead" after light
     * sleep, because the boot-time read handle gets wedged by the sleep. */
    hw_touch_reinit();
    if (bsp_touch_reinit() != ESP_OK) {
        ESP_LOGE(TAG, "bsp_touch_reinit() failed — touch may stay dead");
    }
    hw_display_unlock();

    /* Re-power the peripherals parked on standby (IMU, audio, SD). */
    hw_unpark_peripherals();

    /* No GPIO wake sources to clear — light sleep was removed, so the
     * CPU never halted and normal input polling never stopped. */
    s_mode = HW_POWER_MODE_ACTIVE;
    ESP_LOGI(TAG, "Active mode restored");
    return ESP_OK;
}

/* Power-off task: darkens the display and drives the AXP2101 to a true
 * power-off — soft PWROFF first (best case: PMIC off, ~40 µA, PWR click
 * wakes), then AXP sleep + DC1 drop (turns the ESP32 off, PWR press wakes
 * via IRQ).  The hardware POK power-off (6 s hold) and REG10 soft-PWROFF are
 * both blocked on this board (POK can't be emulated via GPIO10, soft-PWROFF
 * is gated by an OTP flag REG21 bit1=0), so sleep-mode is the reliable path.
 * Runs in its own task so it never blocks the LVGL/timer tasks. */
static void hw_shutdown_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "!!! FULL SYSTEM SHUTDOWN !!!");

    /* Stop rendering + darken. */
    hw_display_pause_rendering();
    hw_display_set_brightness(0);

    /* Hold PEK (GPIO10) LOW during the power-down so the PMIC never sees a
     * button press and auto-restarts.  After the ESP32 loses power the pin
     * floats (high-Z, no pull) back to released. */
    gpio_config_t pwr_hold = {
        .pin_bit_mask = BIT64(GPIO_NUM_10),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_hold);
    gpio_set_level(GPIO_NUM_10, 0);

    /* PMIC power-off (never returns on success). */
    hw_axp2101_shutdown();
    vTaskDelete(NULL);
}

void hw_shutdown(void)
{
    xTaskCreate(hw_shutdown_task, "hw_shutdown", 4096, NULL, 1, NULL);
}

/* ── Periodic poll ──────────────────────────────────────────────────────── */

void hw_poll_events(void)
{
    /* Poll AXP2101 IRQs for PWR button / VBUS / charging events */
    hw_axp2101_poll_events();
}

/* ── Status dump ───────────────────────────────────────────────────────── */

void hw_dump_status(void)
{
    hw_axp2101_dump_status();

    hw_rtc_time_t t;
    if (hw_rtc_get_time(&t) == ESP_OK) {
        ESP_LOGI(TAG, "RTC: %02d:%02d:%02d %04d-%02d-%02d (wday=%d)",
                 t.hour, t.min, t.sec, 2000+t.year, t.month, t.day, t.wday);
    }
}
