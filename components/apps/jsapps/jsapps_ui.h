/*
 * Watch OS — JavaScript app launcher.
 *
 * A simple full-screen grid listing every JS app found on the SD card
 * (icons decoded to PSRAM).  Tapping an app launches it via the JS runner;
 * GPIO0 while an app is running returns to this launcher, and GPIO0 from
 * the launcher itself exits back to the watch menu.
 */
#ifndef JSAPPS_UI_H
#define JSAPPS_UI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void jsapps_ui_create(void);
void jsapps_ui_destroy(void);
bool jsapps_ui_is_active(void);
void jsapps_ui_handle_back(void);

/* Launch a JS app directly (no launcher grid) — used when a JS app icon is
 * tapped straight from the honeycomb menu. */
void jsapps_ui_launch_direct(int index);

#ifdef __cplusplus
}
#endif

#endif /* JSAPPS_UI_H */
