/**
 * @file spinner.h
 * @brief Reusable hexagon-dot spinner system widget.
 *
 * A self-contained indeterminate progress spinner made of six white dots
 * arranged in a hexagon (dot[0]..dot[5]).  Instead of the dots moving,
 * they stay in place while their opacity pattern [100,70,50,30,20,50]
 * rotates around the ring, which reads as a smooth spinning wheel.
 *
 * Any app can create one with spinner_create() and pause/resume it with
 * spinner_set_active().  The spinner owns its animation timer and cleans
 * it up automatically when the object is deleted (e.g. lv_obj_clean() on
 * its parent), so callers never have to manage the timer themselves.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default outer diameter of the spinner in px. */
#define SPINNER_DEFAULT_DIAMETER   76

/**
 * @brief Create a hexagon spinner widget.
 *
 * The six dots are laid out in a hexagon around the centre of the widget
 * and the ring is centred within @p parent (horizontal centring is
 * automatic for flex/column parents).  The animation starts immediately.
 *
 * @param parent    Parent object (e.g. the settings list).
 * @param diameter  Outer diameter of the dot ring in px; use
 *                  SPINNER_DEFAULT_DIAMETER for the standard size.
 * @return The spinner object.
 */
lv_obj_t *spinner_create(lv_obj_t *parent, lv_coord_t diameter);

/**
 * @brief Start or stop the spinning animation.
 *
 * @param spinner  Object returned by spinner_create().
 * @param active   true = spin, false = freeze the current frame.
 */
void spinner_set_active(lv_obj_t *spinner, bool active);

#ifdef __cplusplus
}
#endif
