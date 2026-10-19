/**
 * @file app_ui.h
 * @brief UI management: screen switching, time/battery display, charging screen.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Update the time and date labels on the watch face.
 *        Must be called inside LVGL display lock.
 */
void app_ui_update_time(void);

/**
 * @brief Update the battery arc on the watch face.
 *        Must be called inside LVGL display lock.
 */
void app_ui_update_battery(void);

/**
 * @brief Check charging state and switch screens with debounce.
 *        Must be called inside LVGL display lock.
 */
void app_ui_check_charging(void);

/**
 * @brief Check for pending ANCS notifications and display them.
 *        Must be called from main loop inside LVGL display lock.
 */
void app_ui_check_notifications(void);

#ifdef __cplusplus
}
#endif
