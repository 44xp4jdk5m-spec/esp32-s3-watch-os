/**
 * @file calculator_ui.h
 * @brief iOS-style calculator app UI (ported from ElenixOS com.elenixos.calculator)
 *
 * A faithful port of the ElenixOS calculator application (main.js) to C /
 * LVGL v9 for the 410x502 AMOLED watch. The layout math (pad/gap/cell sizes),
 * colour palette and interaction model are copied 1:1 from the reference so
 * the visual result matches the original design.
 *
 * Design notes:
 *   - The app is a self-contained "screen" (lv_screen) that is created on
 *     entry and fully destroyed on exit, so it never consumes RAM while the
 *     main OS is running.
 *   - All app-owned state is allocated from PSRAM (MALLOC_CAP_SPIRAM) and
 *     freed on exit. LVGL widgets themselves use the PSRAM-backed LVGL
 *     allocator (lv_mem_custom.c).
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the calculator app UI (full-screen, no app header).
 *        Replaces whatever is currently displayed.
 */
void calculator_ui_create(void);

/**
 * @brief Destroy the calculator UI and free all resources (PSRAM + widgets).
 */
void calculator_ui_destroy(void);

/**
 * @brief Check if the calculator UI is currently active.
 */
bool calculator_ui_is_active(void);

/**
 * @brief Handle GPIO0 back button press. Exits to the app menu.
 *        Call from the main loop (NOT from interrupt context).
 */
void calculator_ui_handle_back(void);

#ifdef __cplusplus
}
#endif
