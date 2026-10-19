/*
 * Watch OS — JavaScript bindings (the OS.* namespace).
 *
 * Installs a global `OS` object exposing:
 *   OS.LVGL     — LVGL object/anim/event wrappers (+ timer/pointer)
 *   OS.System   — time, exit
 *   OS.Storage  — persistent integer key/value store (NVS)
 *   OS.Sensor   — QMI8658 IMU (raw accel/gyro + tilt in degrees)
 *
 * The whole engine is used from the LVGL task (event callbacks + timers run
 * inside lv_timer_handler, and the initial eval happens from the launcher's
 * tap callback), so JS is single-threaded.  LVGL object lifetimes are tied to
 * the app screen: deleting the screen frees all JS event refs (via
 * LV_EVENT_DELETE) before the engine is destroyed.
 */
#ifndef JS_BIND_H
#define JS_BIND_H

#include "quickjs/quickjs.h"
#include "quickjs/quickjs-libc.h"
#include "lvgl.h"
#include "nvs.h"

#include "js_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct js_app {
    js_engine_t    *eng;
    JSContext      *ctx;
    lv_obj_t       *scr;             /* app root screen */
    volatile bool   exit_requested;  /* set by OS.System.exit() */
    nvs_handle_t    nvs;
    bool            nvs_open;
} js_app_t;

/* Register the global OS object on the given context. */
void js_bindings_install(JSContext *ctx, js_app_t *app);

/* Stop all LVGL timers owned by `app` and free their JS callbacks.
 * Must be called before the engine/context is destroyed. */
void js_bind_cleanup(js_app_t *app);

#ifdef __cplusplus
}
#endif

#endif /* JS_BIND_H */
