/**
 * @file watch_os.c
 * @brief Watch boot sequence + main loop (moved out of main.c).
 */

#include "watch_os.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
/* LVGL stdio file system driver (declared manually - header not in include path) */
void lv_fs_stdio_init(void);
#include "ui.h"
#include "hw_init.h"
#include "hw_axp2101.h"
#include "hw_display.h"

#include "app_events.h"
#include "app_ui.h"
#include "app_power.h"
#include "app_wifi.h"
#include "app_config.h"
#include "app_lang.h"
#include "app_notification_center.h"
#include "watch_face_picker.h"
#include "watch_face_editor.h"
#include "honeycomb_menu.h"
#include "icon_rearrange.h"
#include "list_view_menu.h"
#include "settings_ui.h"
#include "control_center.h"
#include "flashlight_ui.h"
#include "actionsheet.h"
#include "photos_ui.h"
#include "calculator_ui.h"
#include "noise_ui.h"
#include "activity_ui.h"
#include "jsapps_ui.h"
#include "jsapp_run.h"
#include "app_temperature_monitor.h"
#include "optimized_charging.h"
#include "battery_history.h"
#include "app_usage.h"
#include "power_stats.h"
#include "ui_warning_screen.h"
#include "shutdown_ui.h"
#include "storage_manager.h"
#include "tinyusb_msc.h"
#include "wifi_file_server.h"
#include "ota_manager.h"
#include "wake_on_raise.h"
#include "wake_log.h"
#include "wrist_flick.h"
#include "air_pointer.h"
#include "double_tap.h"
#include "gesture_fusion.h"
#include "gesture_recorder.h"
#include "driver/gpio.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "hw_vibrator.h"
#include "esp_system.h"

/* GPIO numbers for combo-press detection — mirrored from hw_button.c */
#define COMBO_BOOT_GPIO   GPIO_NUM_0
#define COMBO_PWR_GPIO    GPIO_NUM_10

static const char *TAG = "WATCH";

/* Screen that was active before the control center opened (restored on exit). */
static app_screen_t s_screen_before_cc = APP_SCREEN_WATCH_FACE;

/* When an app exits back to the honeycomb menu, black the backlight
 * immediately so the brief watch-face reveal (during the SPI drain before
 * honeycomb_menu_create re-renders and fades back in) is never visible.
 * honeycomb_menu_create's fade-in restores the brightness.  The list-view
 * menu doesn't manage brightness itself, so leave it untouched there. */
static void app_transition_black_for_honeycomb(void)
{
    if (app_menu_get_style() != MENU_STYLE_LIST) {
        hw_display_set_brightness(0);
    }
}

void watch_os_run(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════╗");
    ESP_LOGI(TAG, "║     Watch Firmware v1.0      ║");
    ESP_LOGI(TAG, "╚══════════════════════════════╝");

    /* Suppress noisy component logs during critical boot phase */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("phy", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);

    /* ── 1. Hardware init ──────────────────────────────────────────── */

    hw_init_result_t hw_res;
    ESP_ERROR_CHECK(hw_init_all(&hw_res));

    /* ── 1a. Init vibration motor (GPIO18 PWM) ─────────────────────── */
    {
        esp_err_t vibe_ret = hw_vibrator_init();
        if (vibe_ret != ESP_OK) {
            ESP_LOGW(TAG, "Vibrator init failed (%d)", vibe_ret);
        }
    }

    /* ── 1b. Mount SPIFFS & load persistent config ─────────────────── */
    esp_err_t cfg_ret = app_config_init();
    if (cfg_ret != ESP_OK) {
        ESP_LOGW(TAG, "Config init failed (code %d), using defaults", cfg_ret);
    }

    /* ── 1b0. OTA manager (firmware download/install state) ────────── */
    ota_manager_init();

    /* ── 1b0. Dump the persistent wake log (every light-sleep wake since
     * the last reset/power-cycle, stored in RTC memory + SPIFFS).  This is
     * the reliable way to see how often / why the watch woke, because the
     * USB-Serial/JTAG console drops during light sleep and needs a replug
     * or reset to show anything. ─────────────────────────────────────── */
    wake_log_init();
    wake_log_dump();
    wake_log_persist();

    /* LVGL auto-shows the perf monitor at boot (LV_USE_PERF_MONITOR=y).
     * Hide it here if the user turned it off in Settings — otherwise it
     * reappears after every reboot regardless of the saved setting. */
    if (!g_config.cpu_monitor_enabled) {
        lv_sysmon_hide_performance(NULL);
    }

    /* ── 1b1. Load language strings from SPIFFS ────────────────────── */
    {
        int lang_ret = app_lang_init();
        if (lang_ret != ESP_OK) {
            ESP_LOGW(TAG, "Language init failed (%d), using English", lang_ret);
            app_lang_set(LANG_CODE_EN);
        }
    }

    /* ── 1b2. Sync vibrator config from persisted settings ─────────── */
    {
        hw_vibrator_config_t vcfg;
        vcfg.strength     = g_config.vibrator_strength;
        vcfg.pattern_index = g_config.vibrator_pattern;
        vcfg.duration_ms  = g_config.vibrator_duration_ms;
        hw_vibrator_set_config(&vcfg);
        hw_vibrator_set_muted(g_config.silent_mode);
        ESP_LOGI(TAG, "Vibe: str=%d pat=%d dur=%dms muted=%d",
                 vcfg.strength, vcfg.pattern_index, vcfg.duration_ms,
                 g_config.silent_mode);
    }

    /* ── 1c. Mount SD card via BSP (non-fatal if absent) ──────────── */
    {
        esp_err_t sd_ret = bsp_sdcard_mount();
        if (sd_ret != ESP_OK) {
            ESP_LOGW(TAG, "SD card mount failed (%d) — cache disabled", sd_ret);
        } else {
            ESP_LOGI(TAG, "SD card mounted at /sdcard");
        }
    }

    /* ── 1d. Init storage manager (SD cache, Google Drive config) ──── */
    storage_manager_init();

    /* ── 1e. Init TinyUSB MSC (if enabled — SD card via USB OTG) ───── */
    if (g_config.usb_msc_enabled) {
        esp_err_t usb_ret = tinyusb_msc_init();
        if (usb_ret != ESP_OK) {
            ESP_LOGW(TAG, "TinyUSB MSC init failed (%d)", usb_ret);
        }
    }

    /* ── 1f. Start WiFi file server (if enabled + WiFi connected) ──── */
    if (g_config.wifi_fs_enabled && app_wifi_is_connected()) {
        esp_err_t fs_ret = wifi_file_server_start();
        if (fs_ret != ESP_OK) {
            ESP_LOGW(TAG, "WiFi file server start failed (%d)", fs_ret);
        }
    }

    /* ── 2. Register callbacks + initial battery ───────────────────── */
    app_events_init();

    /* ── 2a0. Init optimized charging (loads 4-week screen usage) ─── */
    optimized_charging_init();
    /* ── 2a0b. Per-app screen-usage tracker (Screen Time page) ────── */
    app_usage_init();
    app_usage_set_js_active_fn(jsapp_run_active_index);
    /* ── 2a0c. Init the battery-drain estimator (Battery page) ────── */
    power_stats_init();

    /* ── 2a. Init wake-on-raise (ESP-DL IMU classifier) ───────────── */
    {
        esp_err_t wr_ret = wake_on_raise_init();
        if (wr_ret != ESP_OK) {
            ESP_LOGW(TAG, "Wake-on-raise init failed (%d)", wr_ret);
        } else {
            ESP_LOGI(TAG, "Wake-on-raise %s",
                     g_config.wake_on_raise_enabled ? "enabled" : "disabled");
        }
    }

    /* ── 2a1. Init wrist-flick back gesture (ESP-DL IMU classifier) ─ */
    {
        esp_err_t wf_ret = wrist_flick_init();
        if (wf_ret != ESP_OK) {
            ESP_LOGW(TAG, "Wrist-flick init failed (%d)", wf_ret);
        } else {
            ESP_LOGI(TAG, "Wrist-flick %s",
                     g_config.wrist_flick_enabled ? "enabled" : "disabled");
            /* The detector task is self-managing: it idles until the feature
             * is enabled AND a sub-screen is active, then loads the model. */
            wrist_flick_start();
        }
    }

    /* ── 2a2. Init IMU gesture recorder (Settings → Experimental) ── */
    {
        esp_err_t gr_ret = gesture_recorder_init();
        if (gr_ret != ESP_OK) {
            ESP_LOGW(TAG, "Gesture recorder init failed (%d)", gr_ret);
        }
    }

    /* ── 2a3. Init motion pointer (air mouse) ─────────────────────── */
    {
        esp_err_t ap_ret = air_pointer_init();
        if (ap_ret != ESP_OK) {
            ESP_LOGW(TAG, "Air pointer init failed (%d)", ap_ret);
        } else if (g_config.air_pointer_enabled) {
            air_pointer_start();
        }
    }

    /* ── 2a4. Init double-tap click gesture (ESP-DL IMU classifier) ─ */
    {
        esp_err_t dt_ret = double_tap_init();
        if (dt_ret != ESP_OK) {
            ESP_LOGW(TAG, "Double-tap init failed (%d)", dt_ret);
        } else {
            /* Self-managing: only samples/classifies while the air pointer
             * is active (it is the pointer's click gesture). */
            double_tap_start();
        }
    }

    /* ── 2a5. Init unified gesture detector (idle/wrist/double-tap) ─ */
    {
        esp_err_t gf_ret = gesture_fusion_init();
        if (gf_ret != ESP_OK) {
            ESP_LOGW(TAG, "Gesture fusion init failed (%d)", gf_ret);
        } else {
            /* Self-managing: takes over gesture recognition ONLY when both
             * wrist-flick + motion pointer are enabled AND gesture.espdl is
             * embedded (otherwise it stays inert and the separate detectors
             * keep working). */
            gesture_fusion_start();
        }
    }

    /* ── 2b. Apply saved brightness ────────────────────────────────── */
    {
        int8_t cur = (int8_t)hw_display_get_brightness();
        int8_t tgt = (int8_t)g_config.brightness;
        if (cur < tgt) {
            for (int i = cur; i <= tgt; i += 2) {
                hw_display_set_brightness((uint8_t)i);
                vTaskDelay(pdMS_TO_TICKS(4));
            }
        } else {
            for (int i = cur; i >= tgt; i -= 2) {
                hw_display_set_brightness((uint8_t)i);
                vTaskDelay(pdMS_TO_TICKS(4));
            }
        }
        g_brightness = tgt;
        ESP_LOGI(TAG, "Brightness restored: %d%%", tgt);
    }

    /* ── 2c. Apply AOD setting ─────────────────────────────────────── */
    app_power_set_aod_enabled(g_config.always_on_display);

    /* ── 2d. Re-apply Low Power Mode (15 FPS render cap) ──────────── */
    if (g_config.low_power_mode) {
        hw_display_set_refr_period_ms(66);   /* ~15 FPS target */
        ESP_LOGI(TAG, "Low Power Mode active — 15 FPS cap");
    }

    /* ── 3. Init UI ────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Initializing UI...");
    /* Mark when the boot UI starts so we can guarantee the boot ring
     * gets a minimum animation time (even with WiFi off there are no
     * milestones, so the ring must still visibly fill). */
    int64_t boot_ui_start_us = esp_timer_get_time();
    hw_display_lock(0);
    ui_init();                       /* boots into black screen + animation */
    hw_display_unlock();
    /* ── 3b. Init LVGL stdio file system (for loading SD card images) ─ */
    lv_fs_stdio_init();

    ESP_LOGI(TAG, "UI ready — boot screen active");

    /* Init notification center early — avoids lazy-init during
     * screen transitions which can trigger WDT via widget-tree walks. */
    app_notification_center_init();

    /* Init watch-face picker (registers the long-press indev callback) */
    watch_face_picker_init();

    /* ── 4. WiFi connect -> NTP -> RTC ──────────────────────────────── */
    /* Restore log levels BEFORE WiFi so you can see what happens */
    esp_log_level_set("wifi", ESP_LOG_INFO);
    esp_log_level_set("phy", ESP_LOG_INFO);
    esp_log_level_set("wifi_init", ESP_LOG_INFO);
    app_wifi_start_provisioning();

    /* ── 4b. Boot complete → transition to watch face ──────────────── */
    /* WiFi deinit fragments DMA heap; let buffers fully recycle before
     * LVGL tries to allocate SPI DMA descriptors.  500ms + a forced
     * render under the lock avoids the "spi transmit color failed"
     * burst at boot. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Guarantee the boot ring gets ~2 s of visible animation.  When WiFi
     * is disabled the ring fills purely on the ramp timer; if the boot
     * already took longer than the minimum this is a no-op. */
    {
        const int64_t min_boot_anim_us = 2000 * 1000;
        int64_t boot_elapsed_us = esp_timer_get_time() - boot_ui_start_us;
        if (boot_elapsed_us < min_boot_anim_us) {
            vTaskDelay(pdMS_TO_TICKS((min_boot_anim_us - boot_elapsed_us) / 1000));
        }
    }

    hw_display_lock(500);
    ui_boot_up_finish();
    g_current_screen = APP_SCREEN_WATCH_FACE;
    app_ui_update_time();
    app_ui_update_battery();

    /* Sync charge state to prevent spurious charging-screen pop
     * right after boot. The PMIC has been polled during boot,
     * so g_is_vbus_present is up-to-date — just align the "was"
     * state and reset the debounce timer. */
    g_was_vbus_present      = g_is_vbus_present;
    g_was_charging          = g_is_charging;
    g_last_charge_change_us = esp_timer_get_time();

    /* Render one frame now while we still hold the lock — this forces
     * LVGL to allocate DMA descriptors before the lock is released,
     * so the first SPI transfer after unlock is a steady-state one. */
    lv_timer_handler();
    hw_display_unlock();
    ESP_LOGI(TAG, "Watch face active");

    /* ── 4c. Start temperature monitor ─────────────────────────────── */
    app_temperature_monitor_start();

    /* ── 5. Main loop ──────────────────────────────────────────────── */
    while (1) {
        app_events_poll();
        app_events_handle_pending_wake();

        /* ── Track active screen time for optimized charging ── */
        optimized_charging_tick();

        /* ── Track per-app screen usage for the Screen Time page ── */
        app_usage_tick();

        /* ── Track battery drain for the Battery page ── */
        power_stats_tick(g_battery_pct, g_is_charging);

        /* ── Always run inactivity checks, even when display is
         * off — this is how touch wakes the screen.  Safe to call
         * without the LVGL display lock (uses BSP brightness API,
         * not LVGL). ─────────────────────────────────────────────── */
        app_power_check_inactivity();

        /* ── Reconcile idle peripheral parking (AOD / Low-Power): parks
         * the audio codecs + SD card + IMU while idle on the watch face,
         * restores them the instant the user is active / opens an app. ── */
        app_power_periph_park_tick();

        /* ══════════════════════════════════════════════════════════════
         * COMBO DETECTION — must run BEFORE individual button actions!
         * The button callback sets g_pending_reset/honeycomb on release,
         * but we need to detect simultaneous press first and suppress
         * those flags before they're acted on.
         * ══════════════════════════════════════════════════════════════ */
        {
            static bool s_combo_triggered = false;
            static int64_t s_combo_debounce_until = 0;
            int64_t now_combo = esp_timer_get_time();

            bool boot_down = (gpio_get_level(COMBO_BOOT_GPIO) == 0);
            bool pwr_down  = (gpio_get_level(COMBO_PWR_GPIO) == 1);

            if (boot_down && pwr_down) {
                if (!s_combo_triggered) {
                    s_combo_triggered = true;

                    /* Clear any individual button flags that may have
                     * been set by the button callback on partial release */
                    g_pending_reset      = false;
                    g_pending_honeycomb  = false;
                    g_pending_nc_dismiss = false;
                    g_pending_wake       = false;
                    g_pending_settings   = false;
                    g_pending_photos     = false;
                    g_pending_calculator = false;
                    g_pending_noise      = false;
                    g_pending_activity   = false;
                    g_pending_jsapps     = false;
                    g_pending_jsapp      = -1;
                    g_pending_rearrange  = false;
                    g_pending_shutdown   = false;
                    g_pending_shutdown_dismiss = false;
                    g_pending_cc_flashlight = false;
                    g_pending_flashlight_exit = false;
                    g_pending_control_center = false;   /* combo ≠ CC open */
                    g_pending_cc_scroll_top = false;
                    g_pending_cc_open_settings = false;

                    if (!g_display_off && g_current_screen != APP_SCREEN_BOOT_UP) {
                        ESP_LOGI(TAG, "══ SCREENSHOT COMBO ══");
                        /* A screenshot writes to the SD card, which may be
                         * unmounted while the watch is idle-parked (AOD /
                         * Low-Power).  Wake + restore so the capture lands. */
                        if (app_power_periphs_parked()) {
                            app_power_wake();
                            app_power_periphs_restore();
                        }
                        storage_manager_trigger_screenshot();
                    } else {
                        ESP_LOGW(TAG, "Screenshot skipped — display off or booting");
                    }
                }
                /* Keep the suppression window alive the whole time both are
                 * held (plus a grace period after release) so the individual
                 * release events never open the control center / trigger a
                 * back after a screenshot combo, no matter how long it's
                 * held. */
                s_combo_debounce_until = now_combo + 600000; /* 600 ms grace */
            } else {
                s_combo_triggered = false;
            }

            /* Suppress individual button actions during the combo grace */
            if (now_combo < s_combo_debounce_until) {
                g_pending_reset      = false;
                g_pending_honeycomb  = false;
                g_pending_nc_dismiss = false;
                g_pending_wake       = false;
                g_pending_settings   = false;
                g_pending_photos     = false;
                g_pending_calculator = false;
                g_pending_noise      = false;
                g_pending_activity   = false;
                g_pending_jsapps     = false;
                g_pending_jsapp      = -1;
                g_pending_rearrange  = false;
                g_pending_shutdown   = false;
                g_pending_shutdown_dismiss = false;
                g_pending_cc_flashlight = false;
                g_pending_flashlight_exit = false;
                g_pending_control_center = false;   /* combo ≠ CC open */
                g_pending_cc_scroll_top = false;
                g_pending_cc_open_settings = false;
            }
        }

        /* GPIO0: on watch face → menu (honeycomb or list based on style),
         *        on menu → back to watch face,
         *        on settings → back (handled by settings_ui internally) */
        if (g_pending_reset) {
            g_pending_reset = false;
            app_events_request_vibe_click();
            if (action_sheet_is_open()) {
                /* GPIO0 dismisses the open modal (action sheet / warning)
                 * instead of navigating back on the page beneath it. */
                hw_display_lock(100);
                action_sheet_close();
                hw_display_unlock();
                ESP_LOGI(TAG, "Modal dismissed (GPIO0)");
            } else if (g_current_screen == APP_SCREEN_CHARGING) {
                /* Charging overlay is showing — ignore GPIO0 (it will
                 * resume normal back behaviour once the overlay hides). */
            } else if (g_current_screen == APP_SCREEN_WATCH_FACE) {
                /* GPIO0 on watch face → enter menu */
                hw_display_lock(100);
                g_current_screen = APP_SCREEN_HONEYCOMB;
                if (app_menu_get_style() == MENU_STYLE_LIST) {
                    list_view_menu_create();
                    ESP_LOGI(TAG, "Watch face → List View (GPIO0)");
                } else {
                    honeycomb_menu_create();
                    ESP_LOGI(TAG, "Watch face → Honeycomb (GPIO0)");
                }
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_HONEYCOMB) {
                hw_display_lock(100);
                if (honeycomb_menu_is_active()) {
                    honeycomb_menu_destroy();
                }
                if (list_view_menu_is_active()) {
                    list_view_menu_destroy();
                }
                g_current_screen = APP_SCREEN_WATCH_FACE;
                app_ui_update_time();
                app_ui_update_battery();
                lv_timer_handler();
                honeycomb_menu_fade_in();
                hw_display_unlock();
                ESP_LOGI(TAG, "Menu → Watch face (GPIO0)");
            } else if (g_current_screen == APP_SCREEN_SETTINGS) {
                /* Settings handles its own back navigation via settings_ui */
                hw_display_lock(100);
                if (settings_ui_is_active()) {
                    /* Trigger back — settings_ui handles page stack internally */
                    settings_ui_handle_back();
                }
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_CONTROL_CENTER) {
                /* GPIO0 exits the control center and restores the screen
                 * that was active before it opened. */
                hw_display_lock(200);
                control_center_handle_back();
                g_pending_cc_settings_target = -1;
                /* If we opened the CC over Settings, its floating header was
                 * hidden while the CC page was up — bring it back now. */
                if (s_screen_before_cc == APP_SCREEN_SETTINGS &&
                    settings_ui_is_active()) {
                    settings_ui_set_header_visible(true);
                }
                g_current_screen = s_screen_before_cc;
                lv_timer_handler();
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_FLASHLIGHT) {
                /* GPIO0 exits the flashlight → back to the control center
                 * (the CC screen stays alive underneath). */
                hw_display_lock(100);
                flashlight_ui_handle_back();
                g_pending_cc_settings_target = -1;
                g_current_screen = APP_SCREEN_CONTROL_CENTER;
                lv_timer_handler();
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_WATCH_FACE_PICKER) {
                /* Back button cancels the picker (keeps current face) */
                hw_display_lock(100);
                watch_face_picker_cancel();
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_WATCH_FACE_EDITOR) {
                /* GPIO0 confirms + exits the editor (colours already saved) */
                hw_display_lock(100);
                watch_face_editor_exit();
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_PHOTOS) {
                /* Photos handles its own back navigation */
                hw_display_lock(100);
                if (photos_ui_is_active()) {
                    photos_ui_handle_back();
                }
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_CALCULATOR) {
                /* Calculator handles its own back navigation */
                hw_display_lock(100);
                if (calculator_ui_is_active()) {
                    calculator_ui_handle_back();
                }
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_NOISE) {
                /* Noise handles its own back navigation */
                hw_display_lock(100);
                if (noise_ui_is_active()) {
                    noise_ui_handle_back();
                }
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_ACTIVITY) {
                /* Activity handles its own back navigation */
                hw_display_lock(100);
                if (activity_ui_is_active()) {
                    activity_ui_handle_back();
                }
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_JSAPPS) {
                /* JS app launcher handles its own back navigation */
                hw_display_lock(100);
                jsapps_ui_handle_back();
                hw_display_unlock();
            } else if (g_current_screen == APP_SCREEN_ICON_REARRANGE) {
                /* GPIO0 in rearrange mode: save the order and return to menu. */
                hw_display_lock(100);
                icon_rearrange_save();
                hw_display_unlock();
                ESP_LOGI(TAG, "Rearrange → save (GPIO0)");
            } else {
                ESP_LOGW(TAG, "Soft reset via GPIO0...");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }

        /* GPIO10 short press → open the control center (one-time blur of
         * the current screen).  Only while a normal screen is shown. */
        if (g_pending_control_center) {
            g_pending_control_center = false;
            if (!g_thermal_shutdown_active &&
                g_current_screen != APP_SCREEN_BOOT_UP &&
                g_current_screen != APP_SCREEN_SHUTDOWN &&
                g_current_screen != APP_SCREEN_CONTROL_CENTER) {
                app_events_request_vibe_click();
                hw_display_lock(500);
                if (control_center_is_active()) {
                    /* A control-center session is ALREADY alive underneath
                     * this Settings page — Settings was deep-linked from the
                     * CC and the CC screen is still waiting below.  GPIO10
                     * here means "back to the CC": collapse Settings back
                     * onto it.  Destroying Settings restores the CC screen
                     * (sw_subpage_destroy) and g_settings_return_to_cc makes
                     * the Settings exit branch below switch back to the CC.
                     * s_screen_before_cc is intentionally NOT touched. */
                    ESP_LOGI(TAG, "Settings over CC → back to control center");
                    if (settings_ui_is_active()) {
                        g_settings_return_to_cc = true;
                        settings_ui_destroy();
                    }
                    hw_display_unlock();
                    /* Fall through: the Settings exit branch (below) flips
                     * g_current_screen back to the control center. */
                } else {
                    s_screen_before_cc = g_current_screen;
                    /* CC is a new page — hide the Settings floating header so
                     * it does not float over the control center (it lives on
                     * the LVGL top layer, which is screen-independent). */
                    if (settings_ui_is_active()) {
                        settings_ui_set_header_visible(false);
                    }
                    control_center_open();
                    g_current_screen = APP_SCREEN_CONTROL_CENTER;
                    lv_timer_handler();
                    hw_display_unlock();
                    ESP_LOGI(TAG, "Control center opened");
                }
            }
        }

        /* GPIO10 short press while the control center is open → scroll to top. */
        if (g_pending_cc_scroll_top) {
            g_pending_cc_scroll_top = false;
            hw_display_lock(100);
            control_center_scroll_top();
            hw_display_unlock();
        }

        /* PWR (GPIO10) held ~2 s → show the shutdown confirmation screen.
         * Wake the display first if it was off / AOD / dimmed. */
        if (g_pending_shutdown) {
            g_pending_shutdown = false;
            app_events_request_vibe_click();
            if (g_display_off || g_aod_active || g_display_dimmed)
                app_power_wake();
            hw_display_lock(100);
            shutdown_ui_show();
            lv_timer_handler();   /* render the shutdown screen now */
            hw_display_unlock();
        }

        /* GPIO0 dismiss notification center */
        if (g_pending_nc_dismiss) {
            g_pending_nc_dismiss = false;
            app_notification_center_dismiss();
        }

        /* ── Settings UI toggle handlers ──────────────────────────── */
        if (g_toggle_ble) {
            g_toggle_ble = false;
            /* BLE toggle: restart ANCS init if enabled */
            ESP_LOGI(TAG, "BLE: %s", g_config.ble_enabled ? "ON" : "OFF");
            app_config_save();
        }
        if (g_toggle_ancs) {
            g_toggle_ancs = false;
            ESP_LOGI(TAG, "ANCS: %s", g_config.ancs_enabled ? "ON" : "OFF");
            app_config_save();
        }

        /* ── Process pending uploads from SD cache (every 30 seconds) ── */
        {
            static int64_t last_pending_check = 0;
            int64_t now_pending = esp_timer_get_time();
            if (now_pending - last_pending_check > 30000000) { /* 30s */
                last_pending_check = now_pending;
                if (storage_manager_pending_count() > 0 &&
                    app_wifi_is_connected() &&
                    g_config.storage_gdrive_enabled) {
                    ESP_LOGI(TAG, "Processing pending uploads...");
                    storage_manager_process_pending_uploads(3);
                }
            }
        }

        int64_t now = esp_timer_get_time();
        bool need_time = false, need_batt = false;

        /* ── Heap monitor: log free RAM every 10 s ── */
        static int64_t last_heap_log = 0;
        if (now - last_heap_log > 10000000) {
            size_t free_dram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            size_t free_dma   = heap_caps_get_free_size(MALLOC_CAP_DMA);
            size_t total_dram = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
            size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
            ESP_LOGI(TAG, "Heap: DRAM %zu/%zu free  PSRAM %zu/%zu free  DMA %zu free",
                     free_dram, total_dram, free_psram, total_psram, free_dma);
            last_heap_log = now;
        }

        static int64_t last_time_upd = 0;
        if (now - last_time_upd > 1000000) { need_time = true; last_time_upd = now; }

        /* Poll battery every 500ms (not 5s!). Also poll immediately
         * when IRQ fires (g_force_batt_check). */
        static int64_t last_batt_upd = 0;
        if (g_force_batt_check || now - last_batt_upd > 500000) {
            hw_axp2101_get_battery_percent(&g_battery_pct);
            hw_axp2101_is_charging(&g_is_charging);
            hw_axp2101_is_vbus_present(&g_is_vbus_present);
            need_batt = true;
            last_batt_upd = now;
            g_force_batt_check = false;

            /* ── 80% charge limit: stop charging at 80%, resume below 78%
             *    (hysteresis so it doesn't flap on/off at the boundary). ── */
            static bool s_charge_limit_active = false;
            if (g_config.charge_limit_80) {
                if (g_battery_pct >= 80 && !s_charge_limit_active) {
                    if (hw_axp2101_charging_disable() == ESP_OK) {
                        s_charge_limit_active = true;
                        ESP_LOGI(TAG, "Charge limit: stopped at %d%%", g_battery_pct);
                    }
                } else if (g_battery_pct < 78 && s_charge_limit_active) {
                    if (hw_axp2101_charging_enable() == ESP_OK) {
                        s_charge_limit_active = false;
                        ESP_LOGI(TAG, "Charge limit: resumed (battery %d%%)", g_battery_pct);
                    }
                }
            } else if (s_charge_limit_active) {
                /* Limit switched off → re-enable charging. */
                if (hw_axp2101_charging_enable() == ESP_OK) {
                    s_charge_limit_active = false;
                    ESP_LOGI(TAG, "Charge limit: re-enabled charging");
                }
            }

            /* ── Optimized charging: adapt current so the battery finishes
             *    ~1 h before the user's typical morning use (0.7 C cap).
             *    Only active when the 80% limit is OFF. ── */
            optimized_charging_update();
        }

        /* ── Record battery level into the 24 h history on each local hour
         * boundary (:00).  While the display is on this poll catches the
         * rollover; while it is off the light-sleep housekeeping timer wakes
         * at :00 and records instead (see app_power.c).  Feeds the Power →
         * Battery bar graph. ──────────────────────────────────────── */
        battery_history_poll();

        /* ── Thermal warning show/hide ──────────────────────────────── */
        if (g_thermal_show_warning || g_thermal_hide_warning) {
            hw_display_lock(200);
            if (g_thermal_show_warning) {
                g_thermal_show_warning = false;
                ui_warning_screen_show(g_cpu_temperature, g_pmic_temperature);
                ESP_LOGW(TAG, "Thermal warning screen shown");
            }
            if (g_thermal_hide_warning) {
                g_thermal_hide_warning = false;
                if (ui_warning_screen_is_active()) {
                    ui_warning_screen_hide();
                    ESP_LOGI(TAG, "Thermal warning screen hidden");
                }
            }
            hw_display_unlock();
        }

        /* ── During thermal shutdown, ignore all button actions ────── */
        if (g_thermal_shutdown_active) {
            g_pending_wake       = false;
            g_pending_reset      = false;
            g_pending_honeycomb  = false;
            g_pending_nc_dismiss = false;
            g_pending_control_center = false;
            g_pending_cc_scroll_top = false;
            g_pending_cc_open_settings = false;
            g_pending_cc_flashlight = false;
            g_pending_flashlight_exit = false;
            g_pending_settings   = false;
            g_pending_photos     = false;
            g_pending_calculator = false;
            g_pending_noise      = false;
            g_pending_activity   = false;
            g_pending_jsapps     = false;
            g_pending_jsapp      = -1;
            g_pending_rearrange  = false;
            g_pending_shutdown   = false;
            g_pending_shutdown_dismiss = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* ── Display off: sleep, let BSP task handle LVGL ──────────── */
        if (g_display_off) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* ══════════════════════════════════════════════════════════════
         * LVGL work — only widget-state updates under the lock.
         * The BSP LVGL task handles ALL lv_timer_handler rendering.
         * If lock times out, skip this iteration entirely.
         * ══════════════════════════════════════════════════════════════ */
        if (!hw_display_lock(50)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Boot screen ──────────────────────────────────────────── */
        if (g_current_screen == APP_SCREEN_BOOT_UP) {
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Shutdown confirmation screen (PWR 2 s hold) ─────────── */
        if (g_current_screen == APP_SCREEN_SHUTDOWN) {
            if (g_pending_shutdown_dismiss) {
                /* GPIO0 → cancel and go back to the previous screen. */
                g_pending_shutdown_dismiss = false;
                app_events_request_vibe_click();
                shutdown_ui_dismiss();
                lv_timer_handler();
            } else if (!shutdown_ui_is_active()) {
                /* Safety: screen was torn down → restore saved state. */
                shutdown_ui_dismiss();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Charging overlay check ──────────────────────────────────
         * Run BEFORE the screen-specific branches so the charging
         * animation appears no matter which screen LVGL is showing
         * (watch face, honeycomb menu, settings, photos, ...).
         * The overlay floats on the top layer, so no screen switching
         * is involved and the previous screen is untouched. */
        app_ui_check_charging();

        /* ── ANCS notification capture ───────────────────────────────
         * Runs on every screen (not just the watch face) so messages
         * arriving while a menu/app is open are still stored.  Adds to
         * the notification store + refreshes the center if it is open. */
        app_ui_check_notifications();

        /* ── Control center ──────────────────────────────────────── */
        if (g_current_screen == APP_SCREEN_CONTROL_CENTER) {
            if (!control_center_is_active()) {
                /* Safety: the overlay was torn down → restore. */
                g_current_screen = s_screen_before_cc;
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            /* Settings button → close CC and open Settings as a normal app. */
            if (g_pending_cc_open_settings) {
                g_pending_cc_open_settings = false;
                ESP_LOGI(TAG, "Control center → Settings app");
                control_center_close();
                if (settings_ui_is_active()) {
                    /* CC was opened over Settings — just reshow its header. */
                    settings_ui_set_header_visible(true);
                } else {
                    settings_ui_create();
                }
                g_current_screen = APP_SCREEN_SETTINGS;
                g_last_activity_us = esp_timer_get_time();
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (g_pending_cc_settings_target >= 0) {
                int target = g_pending_cc_settings_target;
                g_pending_cc_settings_target = -1;
                if (settings_ui_is_active()) {
                    /* The CC was opened over the already-open Settings app.
                     * Drop the CC and jump to the requested page inside it
                     * (Settings keeps its normal back behaviour — it was not
                     * launched from the CC).  Mirrors the Settings-button
                     * branch above. */
                    control_center_close();
                    settings_ui_set_header_visible(true);
                }
                g_current_screen = APP_SCREEN_SETTINGS;
                settings_ui_launch_from_cc((settings_cc_target_t)target);
                ESP_LOGI(TAG, "Control center → Settings (%d)", target);
            }
            if (g_pending_cc_flashlight) {
                g_pending_cc_flashlight = false;
                g_current_screen = APP_SCREEN_FLASHLIGHT;
                flashlight_ui_open();
                lv_timer_handler();
                ESP_LOGI(TAG, "Control center → Flashlight");
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Flashlight ───────────────────────────────────────────── */
        if (g_current_screen == APP_SCREEN_FLASHLIGHT) {
            if (g_pending_flashlight_exit) {
                g_pending_flashlight_exit = false;
                flashlight_ui_handle_back();
                g_current_screen = APP_SCREEN_CONTROL_CENTER;
                g_last_activity_us = esp_timer_get_time();
                ESP_LOGI(TAG, "Flashlight → Control center");
            } else if (!flashlight_ui_is_active()) {
                /* Safety: screen was torn down → restore the CC. */
                g_current_screen = APP_SCREEN_CONTROL_CENTER;
                g_last_activity_us = esp_timer_get_time();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Menu (honeycomb or list view) ──────────────────────── */
        if (g_current_screen == APP_SCREEN_HONEYCOMB) {
            /* Long-press on an icon → rearrange mode.  Wait for the finger
             * to lift before tearing the menu down, so LVGL's indev never
             * keeps pointing at a freed object. */
            if (g_pending_rearrange) {
                lv_indev_t *indev = lv_indev_active();
                if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                    hw_display_unlock();
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                g_pending_rearrange = false;
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(150));
                if (!hw_display_lock(200)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                lv_timer_handler();
                if (honeycomb_menu_is_active()) honeycomb_menu_destroy();
                if (list_view_menu_is_active()) list_view_menu_destroy();
                g_current_screen = APP_SCREEN_ICON_REARRANGE;
                icon_rearrange_create();
                lv_timer_handler();
                g_last_activity_us = esp_timer_get_time();
                ESP_LOGI(TAG, "Honeycomb → Rearrange");
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            bool honeycomb_active = honeycomb_menu_is_active();
            bool list_active = list_view_menu_is_active();
            if (!honeycomb_active && !list_active) {
                /* ── Safe screen transition ──────────────────────────
                 * honeycomb_menu_destroy() was just called from an LVGL
                 * event callback.  The old screen's widgets are deleted
                 * but the SPI display driver may still have pending DMA
                 * transfers.  We must:
                 *   1. Flush pending LVGL work while we hold the lock
                 *   2. Release lock so the BSP LVGL task can drain SPI
                 *   3. Wait generously for DMA buffers to recycle
                 *   4. Re-acquire, flush once more, then create new screen
                 *
                 * Without this 3-stage drain, stale DMA descriptors from
                 * the old screen collide with the new screen's allocations
                 * → "spi transmit color failed" on SH8601. ─────────── */
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(150));   /* generous: let BSP task drain SPI */
                if (!hw_display_lock(200)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                lv_timer_handler();               /* final flush before new screen */

                if (g_pending_settings) {
                    g_pending_settings = false;
                    app_events_request_vibe_click();
                    g_current_screen = APP_SCREEN_SETTINGS;
                    settings_ui_create();
                    ESP_LOGI(TAG, "Menu → Settings UI");
                } else if (g_pending_photos) {
                    g_pending_photos = false;
                    app_events_request_vibe_click();
                    g_current_screen = APP_SCREEN_PHOTOS;
                    photos_ui_create();
                    ESP_LOGI(TAG, "Menu → Photos UI");
                } else if (g_pending_calculator) {
                    g_pending_calculator = false;
                    app_events_request_vibe_click();
                    g_current_screen = APP_SCREEN_CALCULATOR;
                    calculator_ui_create();
                    ESP_LOGI(TAG, "Menu → Calculator UI");
                } else if (g_pending_noise) {
                    g_pending_noise = false;
                    app_events_request_vibe_click();
                    g_current_screen = APP_SCREEN_NOISE;
                    noise_ui_create();
                    ESP_LOGI(TAG, "Menu → Noise UI");
                } else if (g_pending_activity) {
                    g_pending_activity = false;
                    app_events_request_vibe_click();
                    g_current_screen = APP_SCREEN_ACTIVITY;
                    activity_ui_create();
                    ESP_LOGI(TAG, "Menu → Activity UI");
                } else if (g_pending_jsapps) {
                    g_pending_jsapps = false;
                    app_events_request_vibe_click();
                    g_current_screen = APP_SCREEN_JSAPPS;
                    jsapps_ui_create();
                    ESP_LOGI(TAG, "Menu → JS Apps");
                } else if (g_pending_jsapp >= 0) {
                    int idx = g_pending_jsapp;
                    g_pending_jsapp = -1;
                    app_events_request_vibe_click();
                    g_current_screen = APP_SCREEN_JSAPPS;
                    jsapps_ui_launch_direct(idx);
                    ESP_LOGI(TAG, "Menu → JS app %d", idx);
                } else {
                    g_current_screen = APP_SCREEN_WATCH_FACE;
                    app_ui_update_time();
                    app_ui_update_battery();
                }
                lv_timer_handler();
                honeycomb_menu_fade_in();
                g_last_activity_us = esp_timer_get_time();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Settings UI ───────────────────────────────────────────── */
        if (g_current_screen == APP_SCREEN_SETTINGS) {
            if (!settings_ui_is_active()) {
                if (g_settings_return_to_cc) {
                    /* Settings was launched from the control center; its
                     * destroy already restored the CC screen underneath. */
                    g_settings_return_to_cc = false;
                    g_current_screen = APP_SCREEN_CONTROL_CENTER;
                    g_last_activity_us = esp_timer_get_time();
                    hw_display_unlock();
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                /* Same 3-stage drain as above — settings screen uses
                 * lv_screen_load_anim with auto_del, which frees the
                 * old screen asynchronously. */
                bool to_app = g_pending_settings || g_pending_photos ||
                              g_pending_calculator || g_pending_noise ||
                              g_pending_activity || g_pending_jsapp >= 0;
                /* Only black the screen for the settings → launcher exit;
                 * an app launched from Settings > Apps keeps normal
                 * brightness (like apps launched from the menu). */
                if (!to_app) app_transition_black_for_honeycomb();
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(150));
                if (!hw_display_lock(200)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                lv_timer_handler();

                g_current_screen = APP_SCREEN_HONEYCOMB;
                app_events_request_vibe_click();
                /* An app launched from the Settings > Apps page set a
                 * pending flag — go straight to that app instead of the
                 * launcher.  (Mirrors the menu → app dispatch above.) */
                if (g_pending_photos) {
                    g_pending_photos = false;
                    g_current_screen = APP_SCREEN_PHOTOS;
                    photos_ui_create();
                    ESP_LOGI(TAG, "Settings → Photos UI");
                } else if (g_pending_calculator) {
                    g_pending_calculator = false;
                    g_current_screen = APP_SCREEN_CALCULATOR;
                    calculator_ui_create();
                    ESP_LOGI(TAG, "Settings → Calculator UI");
                } else if (g_pending_noise) {
                    g_pending_noise = false;
                    g_current_screen = APP_SCREEN_NOISE;
                    noise_ui_create();
                    ESP_LOGI(TAG, "Settings → Noise UI");
                } else if (g_pending_activity) {
                    g_pending_activity = false;
                    g_current_screen = APP_SCREEN_ACTIVITY;
                    activity_ui_create();
                    ESP_LOGI(TAG, "Settings → Activity UI");
                } else if (g_pending_jsapp >= 0) {
                    int idx = g_pending_jsapp;
                    g_pending_jsapp = -1;
                    g_current_screen = APP_SCREEN_JSAPPS;
                    jsapps_ui_launch_direct(idx);
                    ESP_LOGI(TAG, "Settings → JS app %d", idx);
                } else if (g_pending_settings) {
                    g_pending_settings = false;
                    g_current_screen = APP_SCREEN_SETTINGS;
                    settings_ui_create();
                    ESP_LOGI(TAG, "Settings → Settings UI");
                } else if (app_menu_get_style() == MENU_STYLE_LIST) {
                    list_view_menu_create();
                    ESP_LOGI(TAG, "Settings → List View");
                } else {
                    honeycomb_menu_create();
                    ESP_LOGI(TAG, "Settings → Honeycomb");
                }
                g_last_activity_us = esp_timer_get_time();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Photos UI ─────────────────────────────────────────────── */
        if (g_current_screen == APP_SCREEN_PHOTOS) {
            if (!photos_ui_is_active()) {
                /* 3-stage drain (same as settings → menu) */
                app_transition_black_for_honeycomb();
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(150));
                if (!hw_display_lock(200)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                lv_timer_handler();

                g_current_screen = APP_SCREEN_HONEYCOMB;
                app_events_request_vibe_click();
                if (app_menu_get_style() == MENU_STYLE_LIST) {
                    list_view_menu_create();
                    ESP_LOGI(TAG, "Photos → List View");
                } else {
                    honeycomb_menu_create();
                    ESP_LOGI(TAG, "Photos → Honeycomb");
                }
                g_last_activity_us = esp_timer_get_time();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Calculator UI ────────────────────────────────────────── */
        if (g_current_screen == APP_SCREEN_CALCULATOR) {
            if (!calculator_ui_is_active()) {
                /* 3-stage drain (same as other apps → menu) */
                app_transition_black_for_honeycomb();
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(150));
                if (!hw_display_lock(200)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                lv_timer_handler();

                g_current_screen = APP_SCREEN_HONEYCOMB;
                app_events_request_vibe_click();
                if (app_menu_get_style() == MENU_STYLE_LIST) {
                    list_view_menu_create();
                    ESP_LOGI(TAG, "Calculator → List View");
                } else {
                    honeycomb_menu_create();
                    ESP_LOGI(TAG, "Calculator → Honeycomb");
                }
                g_last_activity_us = esp_timer_get_time();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Noise UI ─────────────────────────────────────────────── */
        if (g_current_screen == APP_SCREEN_NOISE) {
            if (!noise_ui_is_active()) {
                /* 3-stage drain (same as calculator → menu) */
                app_transition_black_for_honeycomb();
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(150));
                if (!hw_display_lock(200)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                lv_timer_handler();

                g_current_screen = APP_SCREEN_HONEYCOMB;
                app_events_request_vibe_click();
                if (app_menu_get_style() == MENU_STYLE_LIST) {
                    list_view_menu_create();
                    ESP_LOGI(TAG, "Noise → List View");
                } else {
                    honeycomb_menu_create();
                    ESP_LOGI(TAG, "Noise → Honeycomb");
                }
                g_last_activity_us = esp_timer_get_time();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Activity UI ─────────────────────────────────────────── */
        if (g_current_screen == APP_SCREEN_ACTIVITY) {
            if (!activity_ui_is_active()) {
                /* 3-stage drain (same as noise → menu) */
                app_transition_black_for_honeycomb();
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(150));
                if (!hw_display_lock(200)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                lv_timer_handler();

                g_current_screen = APP_SCREEN_HONEYCOMB;
                app_events_request_vibe_click();
                if (app_menu_get_style() == MENU_STYLE_LIST) {
                    list_view_menu_create();
                    ESP_LOGI(TAG, "Activity → List View");
                } else {
                    honeycomb_menu_create();
                    ESP_LOGI(TAG, "Activity → Honeycomb");
                }
                g_last_activity_us = esp_timer_get_time();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── JS app launcher / running JS app ─────────────────────── */
        if (g_current_screen == APP_SCREEN_JSAPPS) {
            if (!jsapps_ui_is_active()) {
                app_transition_black_for_honeycomb();
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(150));
                if (!hw_display_lock(200)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                lv_timer_handler();

                g_current_screen = APP_SCREEN_HONEYCOMB;
                app_events_request_vibe_click();
                if (app_menu_get_style() == MENU_STYLE_LIST) {
                    list_view_menu_create();
                    ESP_LOGI(TAG, "JS Apps → List View");
                } else {
                    honeycomb_menu_create();
                    ESP_LOGI(TAG, "JS Apps → Honeycomb");
                }
                g_last_activity_us = esp_timer_get_time();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Icon rearrange mode ──────────────────────────────────── */
        if (g_current_screen == APP_SCREEN_ICON_REARRANGE) {
            if (!icon_rearrange_is_active()) {
                /* Cancel (tap) or save (GPIO0) tore the screen down — return
                 * to the menu with the (possibly updated) icon order. */
                app_transition_black_for_honeycomb();
                lv_timer_handler();
                hw_display_unlock();
                vTaskDelay(pdMS_TO_TICKS(150));
                if (!hw_display_lock(200)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                lv_timer_handler();

                g_current_screen = APP_SCREEN_HONEYCOMB;
                app_events_request_vibe_click();
                if (app_menu_get_style() == MENU_STYLE_LIST) {
                    list_view_menu_create();
                    ESP_LOGI(TAG, "Rearrange → List View");
                } else {
                    honeycomb_menu_create();
                    ESP_LOGI(TAG, "Rearrange → Honeycomb");
                }
                g_last_activity_us = esp_timer_get_time();
            }
            hw_display_unlock();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ── Watch face / Charging: update widget state ────────────── */
        if (g_current_screen == APP_SCREEN_WATCH_FACE) {
            if (need_time) app_ui_update_time();
            if (need_batt) app_ui_update_battery();
        }

        app_notification_center_process();

        /* Poll the input device for a watch-face long-press (opens the
         * face picker).  Polling, not LVGL event callbacks — reliable. */
        watch_face_picker_process();

        /* Generate the next queued picker preview (spinner → image) so
         * thumbnails fill in without blocking the picker from opening. */
        watch_face_picker_load_next_thumb();

        /* Live tick for the watch-face editor preview (Flux recolour). */
        watch_face_editor_process();

        hw_display_unlock();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
