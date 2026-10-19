/**
 * @file sw_transition.h
 * @brief Page transitions + scroll-scale effects for settings-like apps.
 *
 * Splits the animation logic out of the page builders: every page gets its
 * own LVGL screen, forward navigation slides in from the right, back
 * navigation slides the current page out to the right, and a lightweight
 * timer scales list rows near the viewport bottom (70%→100%) as you scroll.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create a new full-screen black page screen (no scroll, no padding). */
lv_obj_t *sw_screen_create(void);

/**
 * @brief Animate-load a page screen.
 * @param scr   the freshly built page screen
 * @param back  true → OUT_RIGHT (page slides out, parent revealed),
 *              false → OVER_LEFT (new page slides in from the right).
 */
void sw_screen_load(lv_obj_t *scr, bool back);

/** Instant load without animation (initial screen — avoids SPI queue
 *  overflow right after the launcher is destroyed). */
void sw_screen_load_instant(lv_obj_t *scr);

/**
 * @brief Start the scroll-scale effect on @p list (50 ms timer).
 * @return the created timer (pass to sw_scroll_effect_stop).
 */
lv_timer_t *sw_scroll_effect_start(lv_obj_t *list);

/** Delete a scroll-effect timer (NULL-safe). */
void sw_scroll_effect_stop(lv_timer_t *t);

#ifdef __cplusplus
}
#endif
