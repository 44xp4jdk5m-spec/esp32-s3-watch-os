/**
 * @file list_view_menu.h
 * @brief Apple Watch-style vertical list view app menu.
 *
 * A vertically-scrolling list of app icons with labels. Features:
 *  - Rounded icon on the left, app name label on the right.
 *  - Snap-to-center: nearest icon snaps to screen center on release.
 *  - Edge scale/dim effect: items near the screen edges are scaled down,
 *    dimmed, and shifted toward the left edge for a depth effect.
 *  - First and last items can snap to center (no elastic interference).
 *
 * The menu style (honeycomb vs list) is configurable via
 * app_menu_set_style() / app_menu_get_style() so it can be selected
 * from the settings UI later.
 */

#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Menu Style Enum ────────────────────────────────────────────────── */

typedef enum {
    MENU_STYLE_HONEYCOMB = 0,   /**< Original hex-spiral honeycomb menu */
    MENU_STYLE_LIST,            /**< Apple Watch-style vertical list view */
} menu_style_t;

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * @brief Set the active menu style. Must be called before entering the menu.
 * @param style  Desired menu style (MENU_STYLE_HONEYCOMB or MENU_STYLE_LIST).
 */
void app_menu_set_style(menu_style_t style);

/**
 * @brief Get the current menu style.
 * @return The active menu style.
 */
menu_style_t app_menu_get_style(void);

/**
 * @brief Create and launch the list view menu.
 *
 * Creates a vertically-scrolling list of app icons with labels,
 * with snap-to-center and edge dim/scale animation effects.
 */
void list_view_menu_create(void);

/**
 * @brief Destroy the list view menu.
 */
void list_view_menu_destroy(void);

/**
 * @brief Check if the list view menu is currently active.
 * @return true if active, false otherwise.
 */
bool list_view_menu_is_active(void);

#ifdef __cplusplus
}
#endif
