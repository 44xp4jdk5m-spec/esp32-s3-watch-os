/**
 * @file icon_rearrange.h
 * @brief Honeycomb icon rearrange mode (iOS "wiggle mode").
 *
 * Entered by long-pressing an icon in the honeycomb menu (distinct-icons
 * mode only).  Renders the grid flat (no fisheye/zoom), scales all icons to
 * 90%, shakes them, and lets the user drag icons to new slots.  The order is
 * persisted to SPIFFS via app_config on GPIO0 (save) or discarded on a clean
 * tap (cancel).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Create the rearrange screen.  The honeycomb menu must already be gone. */
void icon_rearrange_create(void);

/** Save the working order to config and tear the screen down (GPIO0). */
void icon_rearrange_save(void);

/** True while the rearrange screen is alive. */
bool icon_rearrange_is_active(void);

#ifdef __cplusplus
}
#endif
