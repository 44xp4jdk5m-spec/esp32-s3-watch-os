/**
 * @file noise_ui.h
 * @brief Noise (dB meter) system app — UI shell & public API.
 *
 * A self-contained "screen" app (same lifecycle contract as the other
 * screen apps):
 *
 *   - create()  builds the SquareLine-ported noise UI, starts the mic
 *               sampling task and the LVGL refresh timer.
 *   - destroy() stops the task, deletes the timers/screen and frees every
 *               app-owned buffer (all PSRAM), returning RAM to the OS.
 *   - The main OS never touches this module until the Noise icon is tapped,
 *     so it has zero cost while the watch is idle.
 *
 * Screen: 410x502 AMOLED, ported from SquareLine Studio (see ui/).
 */

#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the noise (dB meter) app UI and start capturing audio.
 *        Replaces whatever screen is currently displayed.
 */
void noise_ui_create(void);

/**
 * @brief Destroy the noise UI and release all resources (PSRAM + widgets).
 */
void noise_ui_destroy(void);

/**
 * @brief Check if the noise UI is currently active.
 */
bool noise_ui_is_active(void);

/**
 * @brief Handle GPIO0 back button press. Exits to the app menu.
 *        Call from the main loop (NOT from interrupt context).
 */
void noise_ui_handle_back(void);

#ifdef __cplusplus
}
#endif
