/**
 * @file settings_ui.h
 * @brief ElenixOS exact-replica System Settings UI.
 *
 * Provides the full settings page tree:
 *   - Bluetooth
 *   - Display (brightness slider, AOD, wake settings)
 *   - Notifications
 *   - Sound & Haptics (mute, volume slider, haptics)
 *   - Password (enable, change, simple mode)
 *   - Apps (installed app list)
 *   - General (language, device info)
 *
 * Designed for LVGL v9 on 410x502 AMOLED display.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the settings UI on the active screen.
 *
 * Replaces whatever is currently displayed.  Provides a scrollable
 * list of settings categories with round icon buttons.
 */
void settings_ui_create(void);

/**
 * @brief Destroy the settings UI and free all resources.
 */
void settings_ui_destroy(void);

/**
 * @brief Check if the settings UI is currently active.
 */
bool settings_ui_is_active(void);

/**
 * @brief Handle GPIO0 back button press.
 *
 * Goes back to the previous page.  If already at the main page,
 * exits the settings app and returns to the honeycomb menu.
 * Call from the main loop (NOT from interrupt context).
 */
void settings_ui_handle_back(void);

/**
 * @brief Show or hide the floating settings header (top layer).
 *
 * Used when a full-screen overlay such as the keyboard takes over, so
 * the settings header (back button, clock, title) does not float on top
 * of the overlay.  Call with false before opening the overlay and true
 * when it closes.
 *
 * @param visible true to show, false to hide.
 */
void settings_ui_set_header_visible(bool visible);

/* ── Control-center deep link ──────────────────────────────────────── */

/** Deep-link targets the control center can jump straight into. */
typedef enum {
    SETTINGS_CC_DISPLAY    = 0,   /* Settings > Display */
    SETTINGS_CC_BRIGHTNESS = 1,   /* Settings > Display > Brightness */
    SETTINGS_CC_VOLUME     = 2,   /* Settings > Sound & Haptics */
    SETTINGS_CC_BATTERY    = 3,   /* Settings > Power > Battery */
} settings_cc_target_t;

/**
 * @brief Jump to @p target's settings page from the control center.
 *
 * If the settings UI is not open yet it is created and deep-linked to
 * @p target: backing out of that page (GPIO0 or the header back button)
 * destroys the settings UI and restores the control center screen
 * underneath it (g_settings_return_to_cc is set so the shell knows).
 *
 * If the settings UI is ALREADY open (the CC was opened over a running
 * Settings app), the shell has already dropped the CC — this then just
 * navigates the existing settings to @p target with normal back behaviour
 * (Settings was not launched from the CC, so back walks the page tree and
 * exits to the launcher as usual).
 */
void settings_ui_launch_from_cc(settings_cc_target_t target);

#ifdef __cplusplus
}
#endif
