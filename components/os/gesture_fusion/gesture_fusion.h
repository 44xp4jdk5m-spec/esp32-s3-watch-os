/**
 * @file gesture_fusion.h
 * @brief Unified 3-state gesture detector (idle / wrist / double-tap).
 *
 * When BOTH the wrist-flick "back" gesture AND the motion pointer are
 * enabled, this module replaces the two separate ESP-DL detectors with a
 * SINGLE model (`gesture.espdl`) that classifies each 1.5 s IMU window
 * into one of three states:
 *
 *     idle       -> nothing
 *     wrist      -> back action   (haptic + g_pending_reset, like wrist_flick)
 *     double-tap -> click at the cursor (air_pointer_click, like double_tap)
 *
 * One window, one inference, one output — exactly the "one model" idea.
 * While it is active the wrist_flick and double_tap tasks pause (they are
 * gated on gesture_fusion_is_active()).
 *
 * The module is inert unless `gesture_.espdl` is embedded (train with
 * tinyml_mpu/espdl_model/train_gesture.py and drop it into
 * gesture_fusion/model/gesture.espdl).  Without the model the separate
 * detectors keep working exactly as before.
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
esp_err_t gesture_fusion_init(void);

/**
 * @brief Start the fused detector task.
 * @return ESP_ERR_NOT_FOUND if gesture.espdl is not embedded.
 */
esp_err_t gesture_fusion_start(void);

/**
 * @brief Stop the fused detector task.  Safe from any task.
 */
void gesture_fusion_stop(void);

/**
 * @brief True while the fused detector is taking over gesture recognition
 *        (both features enabled + sub-screen + display fully on + model
 *        embedded).  Used by wrist_flick / double_tap to pause themselves.
 */
bool gesture_fusion_is_active(void);

#ifdef __cplusplus
}
#endif
