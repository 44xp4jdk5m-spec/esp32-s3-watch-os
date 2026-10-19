/**
 * @file app_notification_center.h
 * @brief Notification center with blur overlay triggered by top-edge swipe.
 *
 * Gesture detection is custom (not LVGL gestures):
 *   - Touch must start at y ∈ [0, 20] (top 20px)
 *   - As finger drags down, a dark overlay fades in proportionally
 *   - Max opacity at y = BSP_LCD_V_RES / 2 (half screen, y=251)
 *   - Release past half → overlay stays (notification center shown)
 *   - Release before half → overlay animates back (dismissed)
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the notification center overlay and gesture detector.
 *        Must be called after ui_init().
 */
void app_notification_center_init(void);

/**
 * @brief Process touch input and update the blur overlay.
 *        Must be called from the main loop inside LVGL display lock,
 *        AFTER lv_timer_handler().
 */
void app_notification_center_process(void);

/**
 * @brief Check if notification center is currently fully open.
 */
bool app_notification_center_is_open(void);

/**
 * @brief Force-dismiss the notification center (used by back button).
 */
void app_notification_center_dismiss(void);

/**
 * @brief Rebuild the notification list from the store.
 *        Call whenever a notification is added/removed/cleared.
 *        Safe to call when the center is closed (no-op then).
 */
void app_notification_center_refresh(void);

#ifdef __cplusplus
}
#endif
