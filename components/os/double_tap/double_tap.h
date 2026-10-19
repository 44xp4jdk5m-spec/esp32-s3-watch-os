/**
 * @file double_tap.h
 * @brief Double-tap "click" gesture detector (ESP-DL) for the air pointer.
 *
 * The double-tap gesture (tap your index finger and thumb together twice) is
 * a DISTINCT gesture from the wrist-flick "back" — it has its own trained
 * ESP-DL model (tap.espdl, from gesture_01.csv vs the shared notflick idle
 * data) and its own detector task.  On a confident double-tap it asks the
 * air pointer to click at the current cursor position.
 *
 * This is a system-level module living in components/os/ (like wrist_flick).
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
esp_err_t double_tap_init(void);

/**
 * @brief Start the double-tap detector task.
 *
 * The task self-gates on the air pointer being active (both use the IMU):
 * it only samples/classifies while the motion pointer is running and the
 * display is fully on.
 */
esp_err_t double_tap_start(void);

/**
 * @brief Stop the detector task.  Safe to call from any task.
 */
void double_tap_stop(void);

/**
 * @brief Whether the detector task is currently running.
 */
bool double_tap_is_active(void);

#ifdef __cplusplus
}
#endif
