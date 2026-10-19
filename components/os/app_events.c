/**
 * @file app_events.c
 * @brief Shared state + PMIC/button event callbacks.
 */

#include "app_events.h"
#include "app_ble_ancs.h"
#include "app_config.h"
#include "app_wifi.h"
#include "hw_axp2101.h"
#include "hw_button.h"
#include "hw_init.h"
#include "app_power.h"
#include "hw_vibrator.h"
#include "esp_log.h"

static const char *TAG = "APP_EVT";

/* ── Global state definitions ──────────────────────────────────────── */

int            g_battery_pct           = 100;
bool           g_is_charging           = false;
bool           g_is_vbus_present       = false;
bool           g_was_charging          = false;
bool           g_was_vbus_present      = false;
int64_t        g_last_charge_change_us = 0;
app_screen_t   g_current_screen        = APP_SCREEN_BOOT_UP;
bool           g_charging_anim_active  = false;
int64_t        g_last_activity_us      = 0;
bool           g_display_dimmed        = false;
bool           g_display_off           = false;
bool           g_aod_active            = false;
uint8_t        g_brightness            = 80;
bool           g_pending_wake          = false;
bool           g_pending_reset         = false;
bool           g_pending_honeycomb     = false;
bool           g_pending_control_center = false;
bool           g_pending_cc_scroll_top = false;
bool           g_settings_return_to_cc = false;
int            g_pending_cc_settings_target = -1;
bool           g_pending_cc_flashlight = false;
bool           g_pending_flashlight_exit = false;
bool           g_pending_cc_open_settings = false;
bool           g_pending_shutdown      = false;
bool           g_pending_shutdown_dismiss = false;
bool           g_pending_nc_dismiss    = false;
bool           g_force_batt_check      = false;
bool           g_pending_settings      = false;
bool           g_pending_photos        = false;
bool           g_pending_calculator    = false;
bool           g_pending_noise         = false;
bool           g_pending_activity      = false;
bool           g_pending_jsapps        = false;
int            g_pending_jsapp         = -1;
bool           g_pending_rearrange     = false;
bool           g_toggle_wifi           = false;
bool           g_toggle_ble            = false;
bool           g_toggle_ancs           = false;
bool           g_pending_screenshot    = false;
volatile bool  g_pending_vibe_click    = false;
bool           g_pending_brightness_change = false;
uint8_t        g_new_brightness        = 80;

float          g_cpu_temperature       = -1.0f;
float          g_pmic_temperature      = -1.0f;
bool           g_thermal_shutdown_active = false;
bool           g_thermal_show_warning  = false;
bool           g_thermal_hide_warning  = false;

/* ── AXP2101 event callback ─────────────────────────────────────────── */

static void axp_event_handler(axp2101_event_t event, void *user_data)
{
    /* IMPORTANT: This handler does NOT modify g_is_charging or g_was_charging.
     * Those are owned exclusively by the main loop poll. The IRQ's only job
     * is to reset the debounce timer so the main loop re-checks immediately. */
    static int64_t s_last_check_us = 0;
    int64_t now = esp_timer_get_time();

    switch (event) {
    case AXP2101_EVENT_CHARGING_START:
    case AXP2101_EVENT_VBUS_INSERT:
        if (now - s_last_check_us < 300000) return;  /* 300ms debounce */
        s_last_check_us = now;
        ESP_LOGI(TAG, "Charging IRQ");
        g_force_batt_check = true;  /* poll battery NOW, don't wait */
        break;
    case AXP2101_EVENT_CHARGING_DONE:
    case AXP2101_EVENT_VBUS_REMOVE:
        if (now - s_last_check_us < 300000) return;
        s_last_check_us = now;
        ESP_LOGI(TAG, "Unplug IRQ");
        g_force_batt_check = true;
        break;
    case AXP2101_EVENT_PWR_SHORT_PRESS:
    case AXP2101_EVENT_PWR_LONG_PRESS:
        /* Forward to button subsystem for wake-on-press */
        hw_button_handle_axp_event(event);
        if (event == AXP2101_EVENT_PWR_LONG_PRESS) {
            /* Instead of hard power-off, wake the display (if needed) and
             * ask the main loop to show the shutdown confirmation screen. */
            ESP_LOGI(TAG, "PWR long press -> shutdown screen");
            g_pending_wake = true;
            g_pending_shutdown = true;
        } else {
            ESP_LOGI(TAG, "PWR short press");
        }
        break;
    default:
        break;
    }
}

/* ── Button callback ────────────────────────────────────────────────── */

/* Combo suppression: when the callback fires (on button release), check
 * if the OTHER button is still held down.  If both are down simultaneously
 * (i.e. one was just released but the other is still pressed), suppress
 * the individual button action — the main loop's combo detector will
 * handle the screenshot trigger instead. */

#include "driver/gpio.h"
#define COMBO_BOOT_GPIO   GPIO_NUM_0
#define COMBO_PWR_GPIO    GPIO_NUM_10

static bool s_combo_was_active = false;

static void button_event_handler(hw_button_id_t btn, hw_button_event_t evt,
                                  void *user_data)
{
    /* Button callback runs in FreeRTOS timer context (Tmr Svc).
     * Do NOT call any blocking or stack-heavy functions here.
     * Set flags; the main loop does the actual work. */
    g_last_activity_us = esp_timer_get_time();

    /* ── Combo suppression: if both buttons are down, skip individual actions ── */
    bool boot_down = (gpio_get_level(COMBO_BOOT_GPIO) == 0);
    bool pwr_down  = (gpio_get_level(COMBO_PWR_GPIO) == 1);

    if (boot_down && pwr_down) {
        s_combo_was_active = true;
        return;  /* main loop will take the screenshot */
    }

    /* If we just came out of a combo, suppress one more release event.
     * This prevents the remaining button's release from triggering an action. */
    if (s_combo_was_active) {
        s_combo_was_active = false;
        /* One of the buttons was just released from the combo.
         * Check: if the OTHER button is still down, the user only intended
         * a combo, not an individual press. */
        if (boot_down || pwr_down) {
            return;  /* the other button is still held — still in combo */
        }
    }

    /* ── Individual button actions ────────────────────────────────── */
    if (btn == HW_BTN_BOOT && evt == HW_BTN_EVENT_CLICK) {
        /* GPIO0: exit AOD / wake if display off/dimmed; otherwise back/soft-reset */
        extern bool app_notification_center_is_open(void);
        if (g_aod_active || g_display_off || g_display_dimmed) {
            g_pending_wake = true;
        } else if (app_notification_center_is_open()) {
            g_pending_nc_dismiss = true;
        } else if (g_current_screen == APP_SCREEN_HONEYCOMB ||
                   g_current_screen == APP_SCREEN_SETTINGS ||
                   g_current_screen == APP_SCREEN_PHOTOS ||
                   g_current_screen == APP_SCREEN_CALCULATOR ||
                   g_current_screen == APP_SCREEN_NOISE ||
                   g_current_screen == APP_SCREEN_ACTIVITY ||
                   g_current_screen == APP_SCREEN_WATCH_FACE_PICKER ||
                   g_current_screen == APP_SCREEN_WATCH_FACE_EDITOR ||
                   g_current_screen == APP_SCREEN_ICON_REARRANGE ||
                   g_current_screen == APP_SCREEN_JSAPPS ||
                   g_current_screen == APP_SCREEN_CONTROL_CENTER ||
                   g_current_screen == APP_SCREEN_FLASHLIGHT) {
            g_pending_reset = true;  /* main loop handles → back to menu */
        } else if (g_current_screen == APP_SCREEN_SHUTDOWN) {
            /* GPIO0 while the shutdown screen is showing → cancel it. */
            g_pending_shutdown_dismiss = true;
        } else if (g_current_screen == APP_SCREEN_WATCH_FACE) {
            g_pending_reset = true;
        }
    } else if (btn == HW_BTN_PWR && evt == HW_BTN_EVENT_LONG_PRESS) {
        /* GPIO10 held ~2 s → show the shutdown confirmation screen.
         * Fired by BOTH the AXP PKEY long-press IRQ (via hw_button_handle_axp_event)
         * and the hw_button.c GPIO-poll fallback (on release after >2 s) — both
         * funnel through here, so this single branch covers the reliable path.
         * Wakes the display first if needed; the main loop shows the screen.
         * WARNING: Tmr Svc context (2KB stack) — flags only, no ESP_LOGI! */
        g_pending_wake = true;
        g_pending_shutdown = true;
    } else if (btn == HW_BTN_PWR && evt == HW_BTN_EVENT_CLICK) {
        /* GPIO10: exit AOD / wake if display off/dimmed, otherwise open the
         * control center (or, when it is already open, scroll it to top).
         * WARNING: this runs in Tmr Svc context (2KB stack) — no ESP_LOGI! */
        if (g_aod_active || g_display_off || g_display_dimmed) {
            g_pending_wake = true;
        } else if (g_current_screen == APP_SCREEN_CONTROL_CENTER) {
            g_pending_cc_scroll_top = true;
        } else if (g_current_screen == APP_SCREEN_FLASHLIGHT) {
            /* flashlight sits on top of the control center → go back to it */
            g_pending_flashlight_exit = true;
        } else {
            g_pending_control_center = true;
        }
    }
}

/* ── Init ───────────────────────────────────────────────────────────── */

void app_events_init(void)
{
    hw_axp2101_register_event_callback(axp_event_handler, NULL);
    hw_button_register_callback(button_event_handler, NULL);

    /* Initial readings.
     * Set g_was_vbus_present OPPOSITE so the rising-edge detector
     * fires at boot if USB is already plugged in (even if battery
     * is full and charger shows "done"). */
    hw_axp2101_get_battery_percent(&g_battery_pct);
    hw_axp2101_is_charging(&g_is_charging);
    hw_axp2101_is_vbus_present(&g_is_vbus_present);
    g_was_charging = !g_is_charging;
    g_was_vbus_present = !g_is_vbus_present;

    /* Start debounce timer NOW so charging screen can't trigger
     * during the first 5 seconds of boot (avoids framebuffer
     * corruption from lv_screen_load_anim() racing LVGL init). */
    g_last_charge_change_us = esp_timer_get_time();
    g_last_activity_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Battery: %d%% %s", g_battery_pct,
             g_is_charging ? "(charging)" : "");

    /* Init BLE ANCS for iPhone notifications */
    esp_err_t ret = app_ble_ancs_init("ESP32-Watch");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE ANCS init failed: %s", esp_err_to_name(ret));
    }
}

void app_events_poll(void)
{
    hw_poll_events();
}

void app_events_request_vibe_click(void)
{
    g_pending_vibe_click = true;
}

void app_events_handle_pending_wake(void)
{
    if (g_pending_wake) {
        g_pending_wake = false;
        app_power_wake();
    }
    if (g_pending_vibe_click) {
        g_pending_vibe_click = false;
        hw_vibrator_click();
    }
}
