/**
 * @file ui_warning_screen.h
 * @brief Programmatic thermal warning screen (replaces tem/ui_warning.c).
 *
 * Displays a full-screen warning when CPU or PMIC temperature exceeds
 * the configured threshold.  Shows a red thermometer icon, "High
 * Temperature" title, descriptive text, and the current temperatures.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create and show the thermal warning screen.
 *
 * Must be called from the LVGL task context.  Loads the screen
 * with a fade animation.
 *
 * @param cpu_temp   Current CPU temperature (°C), or -1 if unavailable.
 * @param pmic_temp  Current PMIC temperature (°C), or -1 if unavailable.
 */
void ui_warning_screen_show(float cpu_temp, float pmic_temp);

/**
 * @brief Destroy the warning screen and return to the previous screen.
 *
 * Must be called from the LVGL task context.
 */
void ui_warning_screen_hide(void);

/**
 * @brief Check whether the warning screen is currently active.
 */
bool ui_warning_screen_is_active(void);

#ifdef __cplusplus
}
#endif
