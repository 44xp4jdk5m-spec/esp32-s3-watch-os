/**
 * @file watch_face_flux.h
 * @brief Flux watch-face preview API (for the watch-face editor).
 *
 * Flux can run a SECOND, live scaled-down instance inside a container
 * (the editor's preview) at the same time as the active face screen.
 * The editor uses these functions to create/recolour/update/destroy that
 * preview instance.
 */
#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque Flux instance (defined in watch_face_flux.c). */
typedef struct flux_inst flux_inst_t;

/** Build a live Flux instance as a child of @p parent (410x502, centered).
 *  Returns the instance, or NULL on failure. */
flux_inst_t *flux_preview_create(lv_obj_t *parent);

/** Destroy a preview instance and free its PSRAM. NULL-safe. */
void flux_preview_destroy(flux_inst_t *f);

/** Root object of a preview instance (for scaling/clipping). */
lv_obj_t *flux_preview_root(flux_inst_t *f);

/** Recolour the two Flux panels live (0xRRGGBB). */
void flux_preview_set_colors(flux_inst_t *f, uint32_t top_rgb, uint32_t bot_rgb);

/** Advance the live preview (seconds sweep + digits). Call ~1/s. */
void flux_preview_update(flux_inst_t *f, const char *time_str, const char *date_str);

#ifdef __cplusplus
}
#endif
