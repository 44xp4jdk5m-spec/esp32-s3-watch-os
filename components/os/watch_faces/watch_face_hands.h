/**
 * @file watch_face_hands.h
 * @brief "Analog Hands" watch face — Apple-style analog face built from raw
 *        A8 alpha masks of the cropped 42mm hands
 *        (spiffs/usr/share/watchfaces/hands/layers/, see tools/build_hands_face.py).
 *
 * Layers (bottom -> top): hour shadow/hand/inlay, minute shadow/hand/inlay,
 * second shadow/hand, centre pin.  Every mask is drawn with an LVGL
 * image_recolor at COVER so a colour slot tints the whole shape (the pin is
 * recoloured to the background colour slot).
 *
 * Recolourable like Flux, but with four slots instead of two:
 *   slot 0 = background + centre pin
 *   slot 1 = hour hand (+ inlay)
 *   slot 2 = minute hand (+ inlay)
 *   slot 3 = second hand
 * Colours persist in g_config (hands_face_bg/_hour/_min/_sec).
 *
 * Exposes a preview API (hands_preview_*) so the watch-face editor can host a
 * live scaled-down copy, mirroring watch_face_flux.h.
 */
#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque instance (defined in watch_face_hands.c). */
typedef struct hands_inst hands_inst_t;

/** Build a live preview of the face inside @p parent (root = 410x502,
 *  aligned so the caller can scale it with an LVGL transform). */
hands_inst_t *hands_preview_create(lv_obj_t *parent);

/** Tear down a preview created by hands_preview_create(). NULL-safe. */
void hands_preview_destroy(hands_inst_t *f);

/** The preview's root object (for sizing/scaling). */
lv_obj_t *hands_preview_root(hands_inst_t *f);

/** Live-recolour the preview (slot colours 0xRRGGBB). Cheap. */
void hands_preview_set_colors(hands_inst_t *f, uint32_t bg,
                              uint32_t hour, uint32_t min, uint32_t sec);

/** Advance the preview clock (parses "HH:MM", seconds from the RTC).
 *  Preview positions the hands directly (no sweep animation). */
void hands_preview_update(hands_inst_t *f, const char *time_str,
                          const char *date_str);

#ifdef __cplusplus
}
#endif
