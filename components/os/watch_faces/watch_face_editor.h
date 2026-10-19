/**
 * @file watch_face_editor.h
 * @brief Watch-face editor (recolour mode).  Flux = 2 colour slots,
 *        Analog Hands = 4 colour slots (3 split bars).
 *
 * Flow (from the picker):
 *   - On an editable face card, press "Edit" → watch_face_editor_open(id).
 *   - The picker's preview scales + moves into the editor's preview panel
 *     (which holds a LIVE scaled-down copy of the face).
 *   - Colour sections share one vertical snap strip (one centre).  Scrolling
 *     previews the centred colour on its slot; pressing the centred swatch
 *     saves it (max one green-outlined saved colour per section).
 *   - GPIO0 = exit (colours are already saved on each press); the preview
 *     scales to full screen and the face is re-created with the saved colours.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Does this face have a colour editor? (picker gates the Edit button) */
bool watch_face_editor_supported(uint8_t face_id);

/** Open the editor for @p face_id. Call from the picker's Edit button,
 *  under the LVGL display lock. */
void watch_face_editor_open(uint8_t face_id);

/** Per-loop maintenance (live preview tick). Call under the LVGL lock. */
void watch_face_editor_process(void);

/** Exit: scale the preview to full screen, re-create Flux with the saved
 *  colours. Call under the LVGL lock (GPIO0). */
void watch_face_editor_exit(void);

/** Is the editor currently active? */
bool watch_face_editor_is_active(void);

#ifdef __cplusplus
}
#endif
