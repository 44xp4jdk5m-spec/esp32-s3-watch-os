/**
 * @file flashlight_ui.h
 * @brief Full-screen flashlight (GPIO10 control-center button).
 *
 * Three full-screen modes that scroll horizontally and snap:
 *   0 · White steady
 *   1 · White blink (~2 Hz while this mode is centred)
 *   2 · Red steady
 *
 * A bold black HH:MM clock sits top-right (same spot as the Settings
 * header clock) and three index dots sit bottom-centre (active dot dark
 * #242424, inactive #B0B0B0 — the demo look).  Both fade out after 1 s
 * of no touch and reappear on the next touch, resetting the idle timer.
 *
 * The screen is opened OVER the control center (which stays alive as the
 * "previous screen").  Closing it restores the CC.  GPIO0 / GPIO10 while
 * the flashlight is open close it again (return to CC).  Brightness is
 * forced to 100 % on open and restored to g_config.brightness on close.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One-time init (logs only). */
void flashlight_ui_init(void);

/** True while the flashlight screen is active. */
bool flashlight_ui_is_active(void);

/** Build + load the flashlight screen over the current one (the CC).
 *  Called from the main loop under the display lock. */
void flashlight_ui_open(void);

/** Tear down the flashlight and restore the previous screen (the CC).
 *  Called from the main loop under the display lock. */
void flashlight_ui_close(void);

/** GPIO0/GPIO10 while the flashlight is open: close it (back to CC). */
void flashlight_ui_handle_back(void);

#ifdef __cplusplus
}
#endif
