/**
 * @file activity_ui.h
 * @brief Activity app — UI shell & public API.
 *
 * A self-contained "screen" app (same lifecycle contract as the other
 * screen apps):
 *
 *   - create()  builds the four Activity screens (rings / move / exercise
 *               / stand), loads the rings page, starts the live clock
 *               timer and the ring "count-up" animation.
 *   - destroy() stops the timer/animation, deletes every screen and frees
 *               the PSRAM app context, returning RAM to the OS.
 *   - Swiping up/down switches pages in the order
 *               [ activity → move → exercise → stand ].
 *   - The main OS never touches this module until the Activity icon is
 *     tapped, so it has zero cost while the watch is idle.
 *
 * Screen: 410x502 AMOLED, ported from the SquareLine designs in /my_ui
 * (ui_activity / ui_move / ui_exercise; the Stand page is a new sibling).
 */

#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the Activity app UI and load the rings page.
 *        Replaces whatever screen is currently displayed.
 */
void activity_ui_create(void);

/**
 * @brief Destroy the Activity UI and release all resources (PSRAM + screens).
 */
void activity_ui_destroy(void);

/**
 * @brief Check if the Activity UI is currently active.
 */
bool activity_ui_is_active(void);

/**
 * @brief Handle GPIO0 back button press. Exits to the app menu.
 *        Call from the main loop (NOT from interrupt context).
 */
void activity_ui_handle_back(void);

#ifdef __cplusplus
}
#endif
