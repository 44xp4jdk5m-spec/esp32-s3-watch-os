/**
 * @file sw_graph.h
 * @brief Reusable chart widgets for settings cards.
 *
 * Two cards used by the battery page: a circular arc gauge with a big %
 * label, and a 24 h bar-graph card.  Pure LVGL — callers provide the data
 * (the app layer owns the RTC / storage reads).
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A rounded card with an arc gauge + big percentage label.
 * @param pct    battery level 0-100.
 * @param out_arc        (optional) receives the arc object.
 * @param out_pct_label  (optional) receives the big %% label.
 */
void sw_graph_arc_gauge(lv_obj_t *list, int pct, lv_obj_t **out_arc,
                        lv_obj_t **out_pct_label);

/**
 * @brief A rounded card with a right-aligned bar graph.
 * @param pcts        percent values (index 0 = oldest)
 * @param charging    per-entry "is charging" flag (NULL = none)
 * @param count       number of valid entries in pcts/charging
 * @param max_slots   total slots to reserve (newest bar pinned to the
 *                    right edge, empty slots become leading spacers)
 * @param current_hour  local 0-23 hour used to rotate the x-axis labels
 */
void sw_graph_bar_card(lv_obj_t *list, const uint8_t *pcts,
                       const bool *charging, int count, int max_slots,
                       int current_hour);

/* One calendar day of screen-time usage (seconds, broken down by app
 * category).  The card stacks bottom→top as: social, productivity,
 * other, system. */
typedef struct {
    uint32_t social;        /* blue       */
    uint32_t productivity;  /* light blue */
    uint32_t other;         /* orange     */
    uint32_t system;        /* gray       */
} sw_week_day_t;

/**
 * @brief Weekly stacked screen-time card (7 trailing calendar days).
 * @param days      exactly 7 entries (index 0 = oldest … 6 = today).
 * @param letters   7 weekday letters for the x-axis (index 0 = oldest).
 * @param avg_sec   average daily usage → green dotted line (0 = none).
 */
void sw_graph_week_card(lv_obj_t *list, const sw_week_day_t *days,
                        const char *const *letters, uint32_t avg_sec);

#ifdef __cplusplus
}
#endif
