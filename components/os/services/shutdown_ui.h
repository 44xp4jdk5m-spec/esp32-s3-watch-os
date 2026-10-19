/*
 * Watch OS — shutdown / power-off confirmation screen.
 *
 * Shown when the PWR button (GPIO10) is held for ~2 s.  Full-screen black
 * with a Cancel button (top-left) and a power-off slider (bottom-right):
 * dragging the knob right shrinks the track (250 → 106 px) and dims the
 * display; releasing past the midpoint snaps to min and powers the watch
 * off via the AXP2101.
 */
#ifndef SHUTDOWN_UI_H
#define SHUTDOWN_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show the shutdown screen, replacing the current screen.  The previous
 * screen (and its APP_SCREEN_* id) is saved so dismiss() can restore it. */
void shutdown_ui_show(void);

/* Cancel / dismiss: restore the previous screen, brightness and screen id. */
void shutdown_ui_dismiss(void);

bool shutdown_ui_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* SHUTDOWN_UI_H */
