/**
 * @file app_registry.h
 * @brief Unified, auto-read app registry (native + JS script apps).
 *
 * The Settings > Apps page (and anything else) lists every installed app
 * through this one API instead of hard-coding entries in the UI.  Native
 * apps are a fixed table; JS script apps are appended live from the
 * js_app_registry (scanned from the SD card at boot).
 */

#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How a native app is launched (maps to the shell's pending flags). */
typedef enum {
    APP_LAUNCH_SETTINGS,
    APP_LAUNCH_PHOTOS,
    APP_LAUNCH_CALCULATOR,
    APP_LAUNCH_NOISE,
    APP_LAUNCH_ACTIVITY,
} app_launch_kind_t;

/** One app entry (native or script). */
typedef struct {
    const char           *id;        /* unique id, e.g. "photos"           */
    const char           *name;      /* display name                       */
    const lv_image_dsc_t *icon;      /* PNG icon (native) / decoded JS icon
                                      * (may be NULL)                      */
    uint32_t              color;     /* icon circle bg, RGB hex 0xRRGGBB   */
    bool                  is_js;     /* true = SD-card script app          */
    int                   js_index;  /* js_app_registry index (JS only)    */
    app_launch_kind_t     launch;    /* native launch kind (ignored for JS)*/
} app_entry_t;

/**
 * @brief Total number of installed apps (native + scanned JS apps).
 */
int app_registry_count(void);

/**
 * @brief Get app @p index (0-based).
 * @return pointer valid until the next app_registry_get() call (JS entries
 *         are returned from an internal buffer; native entries are static).
 */
const app_entry_t *app_registry_get(int index);

/**
 * @brief Set the pending-launch flag for the app (native) or the pending
 *        JS index (script app).  The shell performs the actual screen
 *        transition once the current app has exited.
 */
void app_registry_request_launch(int index);

#ifdef __cplusplus
}
#endif
