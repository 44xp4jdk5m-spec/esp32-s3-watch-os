/**
 * @file control_center.h
 * @brief Apple-Watch-style control center overlay (GPIO10 short press).
 *
 * A full-screen overlay built over a one-time heavy blur of a screenshot
 * of whatever screen was active when it opened.  GPIO0 exits; GPIO10
 * while open scrolls the button grid back to the top.  Jump buttons
 * (brightness / volume / display / battery) deep-link into Settings and
 * return here on back instead of navigating the settings tree.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One-time init (logs only). */
void control_center_init(void);

/** True while the control center screen is active. */
bool control_center_is_active(void);

/** Snapshot + blur + build + load the control center screen.
 *  Called from the main loop under the display lock. */
void control_center_open(void);

/** Tear down the control center and restore the previous screen.
 *  Called from the main loop under the display lock. */
void control_center_close(void);

/** GPIO0 while open: close + restore the previous screen. */
void control_center_handle_back(void);

/** GPIO10 while open: scroll the button grid back to the top. */
void control_center_scroll_top(void);

#ifdef __cplusplus
}
#endif
