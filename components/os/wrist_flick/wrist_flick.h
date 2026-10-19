/**
 * @file wrist_flick.h
 * @brief Wrist-flick "back" gesture detector (ESP-DL).
 *
 * A small ESP-DL (int8) classifier recognises a wrist-flick gesture from
 * the last 1.5 s of QMI8658 IMU data.  When the user flicks their wrist
 * while actively using a sub-screen (menu / settings / an app), the watch
 * triggers the same "back" action as the GPIO0 button (see watch_os.c).
 *
 * The detector is intentionally scoped to sub-screens only: it does NOT
 * fire on the watch face (where GPIO0 would open the app menu instead).
 *
 * The module is a self-managing background task:
 *   - The model is only loaded into PSRAM while the feature is enabled AND
 *     the display is fully on (not dimmed / AOD / off) AND the current
 *     screen is a sub-screen — so it never competes for RAM during
 *     display-off (wake-on-raise) or AOD.
 *   - On a confident, debounced flick it plays a haptic click and raises
 *     g_pending_reset, which the main loop dispatches as "back".
 *
 * The model file wrist_flick/model/flick.espdl is embedded in .rodata by
 * CMake ONLY if it exists (see os/CMakeLists.txt).  Until the model has
 * been trained (see tinyml_mpu/espdl_model/train_flick.py) the module
 * compiles but reports ESP_ERR_NOT_FOUND from wrist_flick_start().
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create module resources.  Call once during boot (after
 *        app_config_init, like wake_on_raise_init).
 */
esp_err_t wrist_flick_init(void);

/**
 * @brief Start the detector task.  No-op if already running.
 *
 * Loads the quantised ESP-DL model on first use (the task keeps it while
 * the gesture can fire and frees it otherwise).  Returns
 * ESP_ERR_NOT_FOUND if the model has not been embedded (not trained yet).
 */
esp_err_t wrist_flick_start(void);

/**
 * @brief Stop the detector and release the model.
 *        Safe to call from any task.
 */
void wrist_flick_stop(void);

/**
 * @brief True while the detection task is running.
 */
bool wrist_flick_is_running(void);

/**
 * @brief Whether the feature is enabled in the persisted config.
 */
bool wrist_flick_is_enabled(void);

#ifdef __cplusplus
}
#endif
