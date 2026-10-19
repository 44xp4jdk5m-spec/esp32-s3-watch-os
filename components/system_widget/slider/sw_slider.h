/**
 * @file sw_slider.h
 * @brief Sliders used in list pages: a generic −/＋ stepper slider and a
 *        three-step font-size slider with "Aa" steppers + tick marks.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A generic stepper slider (created by sw_slider_add). */
typedef struct {
    lv_obj_t *slider;
    lv_obj_t *plus_btn, *minus_btn;
    lv_obj_t *plus_label, *minus_label;
} sw_slider_t;

/**
 * @brief Add a titled row with − / ＋ round buttons around a slider.
 * @param list       the list container.
 * @param title      small grey title above the row.
 * @param minus_sym  minus glyph (e.g. SW_ICON_REMOVE).
 * @param plus_sym   plus glyph (e.g. SW_ICON_ADD).
 * @return heap-allocated struct (auto-freed when the list is deleted).
 *         NULL on OOM.  Free with sw_slider_free() if not inserted.
 */
sw_slider_t *sw_slider_add(lv_obj_t *list, const char *title,
                           const void *minus_sym, const void *plus_sym);

/** Free the slider struct only (the LVGL objects belong to the list). */
void sw_slider_free(sw_slider_t *s);

/**
 * @brief Add a font-size slider with three discrete steps (Small / Normal /
 *        Big).  Range is fixed 0..2; steppers move by ±1.
 * @param cb  LV_EVENT_VALUE_CHANGED handler on the slider.
 * @return the slider object, or NULL on OOM.
 */
lv_obj_t *sw_font_slider_add(lv_obj_t *list, const char *title,
                             lv_event_cb_t cb);

#ifdef __cplusplus
}
#endif
