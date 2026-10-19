/**
 * @file sw_row.h
 * @brief Shared pressable row-panel primitive for list-style widgets.
 *
 * A full-width rounded panel (SW_ROW_H) with the Apple-Watch press
 * animation (scale 230/256 + darken) and ROW flex flow.  Every row-type
 * widget (entry, icon-btn, toggle, button, ...) builds on this so they all
 * share the same anatomy / press feel.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a full-width pressable row panel on @p list.
 *
 * The panel is SW_ROW_H tall, rounded (SW_LIST_RADIUS), SW_THEME_SECONDARY
 * fill and ROW flex; callers append their own children (labels / icons).
 * The panel is clickable and scales down while pressed.
 */
lv_obj_t *sw_row_create(lv_obj_t *list);

/**
 * @brief Add a round leading icon (coloured circle + Material glyph).
 * @param row  a row created with sw_row_create().
 * @param bg   icon circle colour.
 * @param src  Material Symbol glyph (e.g. SW_ICON_WIFI).
 * @return the icon circle object.
 */
lv_obj_t *sw_row_round_icon(lv_obj_t *row, lv_color_t bg, const void *src);

#ifdef __cplusplus
}
#endif
