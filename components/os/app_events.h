/**
 * @file app_events.h
 * @brief Shared application state and event handling.
 *
 * This module holds the global state that all other app modules need:
 * battery %, charging flag, screen state, inactivity timer.
 *
 * Event callbacks from AXP2101 PMIC and physical buttons update
 * the state here, and other modules (UI, power) read it.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Screen enum ───────────────────────────────────────────────────── */
typedef enum {
    APP_SCREEN_BOOT_UP,
    APP_SCREEN_WATCH_FACE,
    APP_SCREEN_CHARGING,
    APP_SCREEN_HONEYCOMB,   /* Honeycomb icon-select menu */
    APP_SCREEN_SETTINGS,    /* System Settings UI */
    APP_SCREEN_PHOTOS,      /* Photos app */
    APP_SCREEN_CALCULATOR,  /* iOS-style Calculator app */
    APP_SCREEN_NOISE,       /* Noise (dB meter) app */
    APP_SCREEN_ACTIVITY,    /* Activity (rings) app */
    APP_SCREEN_JSAPPS,      /* JS app launcher + running JS app (SD card) */
    APP_SCREEN_WATCH_FACE_PICKER, /* Watch-face gallery (long-press) */
    APP_SCREEN_WATCH_FACE_EDITOR, /* Watch-face editor (recolour, Flux only) */
    APP_SCREEN_ICON_REARRANGE,    /* Honeycomb icon rearrange mode (long-press) */
    APP_SCREEN_SHUTDOWN,          /* Power-off confirmation (PWR 2s hold) */
    APP_SCREEN_CONTROL_CENTER,    /* Control center overlay (GPIO10 short press) */
    APP_SCREEN_FLASHLIGHT,        /* Flashlight screen (CC flashlight button)     */
} app_screen_t;

/* ── Global state (read/write by all modules) ──────────────────────── */

/** Current battery percentage (0–100) */
extern int       g_battery_pct;

/** Is USB VBUS present and charging active? */
extern bool      g_is_charging;

/** Is USB VBUS physically present (plugged in)?  Works even when
 *  battery is full and charger shows "done". */
extern bool      g_is_vbus_present;

/** Previous charging state (for edge detection) */
extern bool      g_was_charging;

/** Previous VBUS state (for edge detection) */
extern bool      g_was_vbus_present;

/** Timestamp of last charge state change (debounce cooldown) */
extern int64_t   g_last_charge_change_us;

/** Current active screen */
extern app_screen_t g_current_screen;

/** Is the charging arc deceleration animation running? */
extern bool      g_charging_anim_active;

/** Timestamp of last touch/button activity (for inactivity timer) */
extern int64_t   g_last_activity_us;

/** Is display currently dimmed to half brightness? */
extern bool      g_display_dimmed;

/** Is display currently fully off? */
extern bool      g_display_off;

/** Is the Always-On Display low-power mode currently active?
 *  Screen stays on, dimmed, panel in ~1Hz idle refresh + pixel shift. */
extern bool      g_aod_active;

/** Current brightness target (0–100) */
extern uint8_t   g_brightness;

/** Button press requested a display wake (set from timer cb, handled in main loop) */
extern bool      g_pending_wake;

/** GPIO0 pressed on watch face → soft reset */
extern bool      g_pending_reset;

/** GPIO10 pressed while display on → enter honeycomb menu (reserved) */
extern bool      g_pending_honeycomb;

/** GPIO10 short press while display on → open the control center */
extern bool      g_pending_control_center;

/** GPIO10 short press while the control center is open → scroll to top */
extern bool      g_pending_cc_scroll_top;

/** Settings app launched from the control center is exiting → return to CC */
extern bool      g_settings_return_to_cc;

/** Pending control-center deep-link target (settings_cc_target_t), -1 = none */
extern int       g_pending_cc_settings_target;

/** Flashlight button in the control center → open the flashlight screen */
extern bool      g_pending_cc_flashlight;

/** GPIO10/GPIO0 while the flashlight is open → close it (back to CC) */
extern bool      g_pending_flashlight_exit;

/** Settings button in the control center → open the Settings app (closes CC) */
extern bool      g_pending_cc_open_settings;

/** PWR (GPIO10) held ~2 s → show the shutdown confirmation screen */
extern bool      g_pending_shutdown;

/** GPIO0 pressed while the shutdown screen is showing → dismiss it */
extern bool      g_pending_shutdown_dismiss;

/** GPIO0 pressed while notification center open → dismiss NC */
extern bool      g_pending_nc_dismiss;

/** Charging IRQ fired — main loop should poll battery immediately */
extern bool      g_force_batt_check;

/** Settings icon clicked in honeycomb — launch settings UI */
extern bool      g_pending_settings;

/** Photos icon clicked in honeycomb — launch photos UI */
extern bool      g_pending_photos;

/** Calculator icon clicked in honeycomb — launch calculator UI */
extern bool      g_pending_calculator;

/** Noise icon clicked in honeycomb — launch noise (dB meter) UI */
extern bool      g_pending_noise;

/** Activity icon clicked in honeycomb — launch Activity UI */
extern bool      g_pending_activity;

/** App Store icon clicked in honeycomb — open the JS app launcher (SD card) */
extern bool      g_pending_jsapps;

/** A specific JS app icon was clicked — launch it directly (index, or -1) */
extern int       g_pending_jsapp;

/** An icon was long-pressed in the honeycomb menu — enter rearrange mode */
extern bool      g_pending_rearrange;

/** Toggle WiFi from settings UI (edge-triggered) */
extern bool      g_toggle_wifi;

/** Toggle BLE from settings UI (edge-triggered) */
extern bool      g_toggle_ble;

/** Toggle ANCS from settings UI (edge-triggered) */
extern bool      g_toggle_ancs;

/** GPIO10 + GPIO0 combo press detected — trigger screenshot + upload */
extern bool      g_pending_screenshot;

/** Trigger a haptic click (button press feedback, processed in main loop) */
extern volatile bool      g_pending_vibe_click;

/** Pending brightness change from settings UI */
extern bool      g_pending_brightness_change;
extern uint8_t   g_new_brightness;

/**
 * @brief Request a haptic click feedback from any task or UI callback.
 *
 * The request is serviced later by the main event handler.
 */
void app_events_request_vibe_click(void);

/* ── Thermal monitoring state ──────────────────────────────────────── */

/** Current ESP32-S3 CPU temperature in °C (-1 if not yet read) */
extern float     g_cpu_temperature;

/** Current AXP2101 PMIC temperature in °C (-1 if not yet read) */
extern float     g_pmic_temperature;

/** True when thermal shutdown is active (WiFi/BT/charging stopped) */
extern bool      g_thermal_shutdown_active;

/** True when we need to show the warning screen (edge-triggered) */
extern bool      g_thermal_show_warning;

/** True when we need to hide the warning screen (edge-triggered) */
extern bool      g_thermal_hide_warning;

/* ── API ────────────────────────────────────────────────────────────── */

/**
 * @brief Register all event callbacks (AXP2101 + buttons).
 *        Must be called once after hw_init_all().
 */
void app_events_init(void);

/**
 * @brief Poll PMIC events (call from main loop).
 */
void app_events_poll(void);

/**
 * @brief If g_pending_wake is set, call app_power_wake() safely from main task.
 *        Must NOT be called from timer/IRQ context.
 */
void app_events_handle_pending_wake(void);

#ifdef __cplusplus
}
#endif
