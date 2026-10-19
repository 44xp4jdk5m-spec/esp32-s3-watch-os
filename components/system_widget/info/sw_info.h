/**
 * @file sw_info.h
 * @brief Auto-height information / key-value panel row.
 *
 * A rounded panel whose height is driven by the text content (NOT the tall
 * fixed SW_ROW_H used by interactive rows) — ideal for read-only status
 * rows such as "Status: Mounted", "Pending uploads: 5" or
 * "Last Charged to 100% · 3h ago", which would otherwise waste ~2/3 of a
 * 100 px row on one short line.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add an auto-height key/value panel to @p list.
 * @param list        the list container.
 * @param label       key text (white, SW_FONT_M, grows to fill).
 * @param value       value text on the right (may be NULL for key only).
 * @param value_color value colour (e.g. SW_COLOR_GREEN / SW_COLOR_RED).
 * @param value_font  value font (NULL -> SW_FONT_S).
 * @return the panel (callers may update children / hide it later).
 */
lv_obj_t *sw_info_add(lv_obj_t *list, const char *label,
                      const char *value, lv_color_t value_color,
                      const lv_font_t *value_font);

#ifdef __cplusplus
}
#endif
