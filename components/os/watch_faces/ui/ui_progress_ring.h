/**
 * @file ui_progress_ring.h
 * @brief Reusable radial progress ring (discrete gray→white bars).
 *
 * A ring of discrete radial bars pointing toward the centre.  Idle bars are
 * gray; as progress advances they turn white clockwise from the top.  Shared
 * by the boot-up screen and the OTA install screen.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a radial progress ring widget.
 * @param parent  Parent object.
 * @param size    Widget size in px (diameter).
 * @param bars    Number of radial bars (0 = default 180).
 * @return The ring object, or NULL on allocation failure.
 */
lv_obj_t *ui_progress_ring_create(lv_obj_t *parent, int32_t size, uint16_t bars);

/**
 * @brief Set progress 0..100.  Bars turn white clockwise from the top.
 * @param ring  Ring object from ui_progress_ring_create().
 * @param pct   Progress percent (clamped 0..100).
 */
void ui_progress_ring_set_progress(lv_obj_t *ring, uint16_t pct);

/**
 * @brief Set the radial bar line width (clamped 1..16, default 3).
 * @param ring   Ring object from ui_progress_ring_create().
 * @param width  Bar line width in px.
 */
void ui_progress_ring_set_bar_width(lv_obj_t *ring, uint8_t width);

#ifdef __cplusplus
}
#endif
