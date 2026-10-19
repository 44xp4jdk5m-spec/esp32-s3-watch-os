/**
 * @file sw_nav_header.h
 * @brief Floating navigation header (back button + title + clock slot).
 *
 * A persistent header on lv_layer_top() so the back button, clock and title
 * never move during page transitions — only the title cross-fades.  The
 * clock label is provided as a slot; the owner creates its own 1-second
 * timer into it (keeps this module free of RTC/clock dependencies).
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sw_header sw_header_t;

/**
 * @brief Create the floating header on the top layer.
 * @param title    initial title text
 * @param back_cb  click handler for the circular back button
 * @param back_ud  user_data delivered to @p back_cb (LV_EVENT_CLICKED)
 */
sw_header_t *sw_header_create(const char *title, lv_event_cb_t back_cb,
                              void *back_ud);

/** Cross-fade the title to @p title (no-op if unchanged). */
void sw_header_set_title(sw_header_t *h, const char *title);

/** Show / hide the whole header (used when a full-screen overlay opens). */
void sw_header_set_visible(sw_header_t *h, bool visible);

/** Whether the header is currently visible. */
bool sw_header_is_visible(const sw_header_t *h);

/** The clock label slot (owner creates its own timer into it). */
lv_obj_t *sw_header_clock_label(sw_header_t *h);

/** The title label (owner may want it, e.g. for language refresh). */
lv_obj_t *sw_header_title_label(sw_header_t *h);

/** Delete the header and cancel any in-flight title animation. */
void sw_header_destroy(sw_header_t *h);

#ifdef __cplusplus
}
#endif
