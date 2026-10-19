/**
 * @file watch_face_picker.h
 * @brief Apple-Watch-style watch face picker.
 *
 * A long-press on the watch face opens a gallery screen with:
 *   - A solid black background.
 *   - A horizontally scrollable carousel of outlined preview cards
 *     (SquareLine layout: 5px gray border panel + inner rounded clip),
 *     fixed size, with snap-to-center paging.
 *   - A face-name label above every card.
 *   - Off-center cards dimmed (SquareLine opa 100).
 *   - An "Edit" button below the carousel (placeholder).
 *   - Scale transitions (Apple-style): entering the picker shrinks the
 *     current face into its preview card; selecting scales the preview
 *     back up to full screen before the face activates.
 *   - Preview thumbnails load asynchronously: cards appear instantly
 *     with a system hexagon spinner, then fill in one at a time (current
 *     first, then neighbours outward).
 *   - Tap the centered card to select it; tap an off-center card to
 *     center it first.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the long-press callbacks (indev + screen-level). Call once
 *  after UI init. */
void watch_face_picker_init(void);

/** Re-attach the screen-level PRESSING callback after screen changes.
 *  Call from the main loop under the LVGL display lock. */
void watch_face_picker_process(void);

/** Generate the next queued preview thumbnail (spinner → image).
 *  Call from the main loop under the LVGL display lock while the picker
 *  is open. */
void watch_face_picker_load_next_thumb(void);

/** Open the picker. Must be called under the LVGL display lock
 *  (LVGL task context). */
void watch_face_picker_open(void);

/** Select a face (by id) and close the picker. LVGL lock required. */
void watch_face_picker_select(uint8_t id);

/** Close the picker without changing the face. LVGL lock required. */
void watch_face_picker_cancel(void);

/** Is the picker currently open? */
bool watch_face_picker_is_open(void);

/* ── Editor integration ───────────────────────────────────────────── */

/** Descriptor of the centred card's preview (or NULL before it loads). */
const lv_img_dsc_t *watch_face_picker_preview_dsc(void);

/** Screen rect of the centred card's preview (inside its outline). */
void watch_face_picker_preview_rect(lv_area_t *out);

/** Close the picker without loading a face (the editor takes over). */
void watch_face_picker_force_close(void);

#ifdef __cplusplus
}
#endif
