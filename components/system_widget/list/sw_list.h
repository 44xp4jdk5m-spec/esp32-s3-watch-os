/**
 * @file sw_list.h
 * @brief Scrollable list container + spacing helpers.
 *
 * The old monolithic sw_list module was split into one folder per widget
 * type (row / title / comment / learn_more / info / entry / icon_btn /
 * button / toggle / slider / picker).  This module now only owns the
 * scroll container that the other row widgets are appended to, plus the
 * vertical spacers used to separate groups.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── List container ────────────────────────────────────────────────── */

/**
 * @brief Create a scrollable settings list on @p parent.
 * @param header_h  Reserved space at the top (e.g. SW_HDR_H) so the
 *                  floating header never overlaps the first row.
 */
lv_obj_t *sw_list_create(lv_obj_t *parent, lv_coord_t header_h);

/** Vertical spacer that keeps groups apart. */
void sw_list_add_placeholder(lv_obj_t *list, lv_coord_t h);

/**
 * @brief Vertical spacer between GROUPS of related widgets.
 *
 * The group gap is 1.5× the normal in-group gap (SW_LIST_GROUP_GAP).
 * The spacer cancels the list's pad_row on both sides so its height is
 * exactly the group gap.  Use this between groups — widgets inside one
 * group just use the normal pad_row gap (no spacer at all).
 */
void sw_list_add_group_gap(lv_obj_t *list);

#ifdef __cplusplus
}
#endif
