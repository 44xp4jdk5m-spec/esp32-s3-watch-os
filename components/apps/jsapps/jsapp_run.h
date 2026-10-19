/*
 * Watch OS — JavaScript app runner.
 *
 * Loads a JS app's main.js from the SD card, creates an isolated QuickJS
 * runtime (PSRAM-backed), installs the OS.* bindings and evaluates the
 * entry script.  All JS runs inside the LVGL task (event callbacks +
 * timers), so the engine is effectively single-threaded.
 */
#ifndef JSAPP_RUN_H
#define JSAPP_RUN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loading result reported through the completion callback. */
typedef enum {
    JSAPP_LOAD_OK = 0,   /* app screen is now active */
    JSAPP_LOAD_FAIL,     /* previous screen was restored */
} jsapp_load_result_t;

typedef void (*jsapp_load_done_cb_t)(jsapp_load_result_t result, void *user_data);

/* Begin loading app `index` asynchronously:
 *  - shows a loading screen (black + app icon + spinning dot ring),
 *  - defers the SD read / engine creation / eval to the LVGL task so the
 *    dot animation actually runs,
 *  - calls `done_cb(result, user_data)` (from the LVGL task) when finished.
 *
 * Returns false if the app index is invalid (nothing is shown).  On success
 * the app screen becomes active; on failure the previous screen is restored.
 * The previous screen must stay alive until `done_cb` fires (the caller
 * deletes it only on JSAPP_LOAD_OK). */
bool jsapp_run_start(int index, jsapp_load_done_cb_t done_cb, void *user_data);

/* True while the loading screen is shown and the deferred load is pending. */
bool jsapp_run_is_loading(void);

/* Cancel an in-flight load (deletes the loading screen and restores the
 * previous screen), or delete the app screen + destroy the JS engine
 * (in that order so the LV_EVENT_DELETE handlers free their JS refs first). */
void jsapp_run_destroy(void);

bool jsapp_run_is_active(void);

/* Index of the JS app currently on screen (-1 = none).  Used by the OS
 * app-usage tracker to attribute screen-on time to the running app. */
int jsapp_run_active_index(void);

/* True when the app called OS.System.exit(). */
bool jsapp_run_exit_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* JSAPP_RUN_H */
