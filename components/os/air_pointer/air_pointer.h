/**
 * @file air_pointer.h
 * @brief Air pointer (motion mouse) — control an on-screen cursor by tilting
 *        the watch, double-tap (on the watch body) to click, and auto-scroll
 *        when the cursor rests near a screen edge.
 *
 * Enabled from Settings -> Accessibility & Gesture ("Motion Pointer").  While
 * enabled the cursor is always visible on the top layer:
 *   - Tilt  (QMI8658 accelerometer -> board angle to earth) moves the cursor.
 *   - Double-tap your index finger + thumb together twice clicks whatever is
 *     under the cursor.  The gesture is recognised by its OWN ESP-DL model
 *     (components/os/double_tap, tap.espdl) which calls air_pointer_click()
 *     -> a second LVGL pointer indev so normal touch still works untouched.
 *   - Resting the cursor near an edge auto-scrolls the scrollable content
 *     under it (same edge-scroll feel as the honeycomb rearrange mode).
 *
 * This is a system-level module: it lives in components/os/ (like
 * wrist_flick) and is independent of any app.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create module resources (semaphore).  Call once at boot.
 */
esp_err_t air_pointer_init(void);

/**
 * @brief Start the air-pointer task + cursor.
 *
 * No-op if already running.  The task self-gates on
 * g_config.air_pointer_enabled and the display state, and creates the cursor
 * overlay + virtual pointer indev on demand.
 */
esp_err_t air_pointer_start(void);

/**
 * @brief Stop the air-pointer task, hide and free the cursor/indev.
 *        Safe to call from any task.
 */
void air_pointer_stop(void);

/**
 * @brief Whether the feature is enabled in the persisted config.
 */
bool air_pointer_is_enabled(void);

/**
 * @brief Whether the air pointer is currently running (task active).
 */
bool air_pointer_is_active(void);

/**
 * @brief Trigger a click (press) at the current cursor position.
 *
 * Called by the double-tap ESP-DL detector (components/os/double_tap).  The
 * virtual pointer indev reports PRESSED for ~AP_CLICK_HOLD_MS so LVGL
 * resolves a normal click at the cursor.  Safe to call from any task.
 */
void air_pointer_click(void);

#ifdef __cplusplus
}
#endif
