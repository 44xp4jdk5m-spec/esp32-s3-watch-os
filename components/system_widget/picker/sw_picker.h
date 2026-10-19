/**
 * @file sw_picker.h
 * @brief Selection pickers for lists: a joined vertical radio picker and a
 *        horizontal segmented picker.
 *
 * Vertical: one rounded panel of N option rows separated by hairline black
 * separators; each row shows an optional leading icon + label + a check
 * tick (right by default, or LEFT for radio style).  Segmented: N equal
 * buttons in a row, the active one filled with the theme primary colour.
 *
 * Both are data-driven: pass an array of {label, icon} options plus the
 * currently selected index and a selection callback.  Inside the callback
 * (LV_EVENT_CLICKED, user_data = option index) call sw_picker_set_selected()
 * to move the highlight.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One picker option. */
typedef struct {
    const char *label;   /**< option text. */
    const void *icon;    /**< optional leading Material glyph (NULL = none). */
} sw_picker_opt_t;

/**
 * @brief Create a vertical joined radio picker.
 * @param list       the list container.
 * @param opts       options array (labels/icons; caller keeps ownership).
 * @param count      number of options.
 * @param selected   initially selected index.
 * @param tick_left  true → check tick on the LEFT (radio style); false →
 *                   tick on the RIGHT.
 * @param sel_cb     selection callback: LV_EVENT_CLICKED, user_data =
 *                   option index.  Call sw_picker_set_selected() inside.
 * @return the picker container.
 */
lv_obj_t *sw_picker_create(lv_obj_t *list, const sw_picker_opt_t *opts,
                           int count, int selected, bool tick_left,
                           lv_event_cb_t sel_cb);

/**
 * @brief Create a horizontal segmented picker (active button filled).
 * @param opts  options array (only @p label is used).
 */
lv_obj_t *sw_picker_create_segmented(lv_obj_t *list,
                                     const sw_picker_opt_t *opts,
                                     int count, int selected,
                                     lv_event_cb_t sel_cb);

/** Highlight option @p idx, clear every other. */
void sw_picker_set_selected(lv_obj_t *picker, int idx);

#ifdef __cplusplus
}
#endif
