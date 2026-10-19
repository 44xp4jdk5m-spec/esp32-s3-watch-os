/*
 * Watch OS — JS app loading screen.
 *
 * Full-screen black overlay with the app icon centered and a ring of
 * rotating dots around it (comet-tail), matching ElenixOS's
 * eos_loading_spinner in infinite (indeterminate) mode.
 */
#ifndef JSAPP_LOADING_H
#define JSAPP_LOADING_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the full-screen loading screen.  `icon` may be NULL (dots only).
 * Returns the screen object; the caller owns it and must delete it once
 * the app screen becomes active. */
lv_obj_t *jsapp_loading_screen_create(const lv_image_dsc_t *icon);

#ifdef __cplusplus
}
#endif

#endif /* JSAPP_LOADING_H */
