/**
 * @file watch_face.h
 * @brief Watch-face abstraction + registry.
 *
 * A "watch face" is a self-contained, full-screen LVGL screen plus a small
 * set of callbacks.  This decouples the app layer (app_ui, main) from any
 * specific face, so adding a new face is just:
 *
 *   1. Write one module that implements `watch_face_t`
 *      (create / destroy / update_time / update_battery / name / id).
 *   2. Register it in `watch_face_registry_init()`.
 *
 * The picker (watch_face_picker) automatically discovers every registered
 * face, snapshots it for the gallery preview, and lets the user switch.
 * No other files need to change.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ────────────────────────────────────────────────────────── */
#define WATCH_FACE_MAX   8   /**< Max faces that can be registered. */

/* ── Built-in face IDs ─────────────────────────────────────────────── */
enum {
    WATCH_FACE_CLASSIC = 0,  /**< SquareLine Studio "Classic" face. */
    WATCH_FACE_MINIMAL,      /**< Simple pure-LVGL "Minimal" face. */
    WATCH_FACE_FLUX,         /**< Animated "Flux" face (seconds-driven bars). */
    WATCH_FACE_PARALLAX,     /**< 3D depth photo face (QMI8658 parallax). */
    WATCH_FACE_HANDS,        /**< "Analog Hands" face (recolourable hands). */
    WATCH_FACE_CALIFORNIA,   /**< "California" face (fullscreen dial + hands). */
    WATCH_FACE_COUNT,        /* number of built-in faces */
};

/**
 * @brief A single watch face.
 */
typedef struct watch_face {
    const char *name;                       /**< Display name (picker footer). */
    uint8_t id;                             /**< Unique ID (persisted in config). */

    /** Create the face's full-screen object (parent = NULL) and return it.
     *  Must be idempotent — called again after destroy(). */
    lv_obj_t *(*create)(void);

    /** Destroy the face's screen. Must be NULL-safe. */
    void (*destroy)(void);

    /** Update the time/date shown on this face. Called under the LVGL lock
     *  every second. `time_str` e.g. "10:08", `date_str` e.g. "Monday, 3 Aug". */
    void (*update_time)(const char *time_str, const char *date_str);

    /** Update battery state on this face. Called under the LVGL lock. */
    void (*update_battery)(int pct, bool charging, bool vbus_present);

    /** Optional — AOD burn-in prevention.  Called (under the LVGL lock) while
     *  Always-On Display mode is active, every AOD pixel-shift interval, with
     *  the next shift offset (dx, dy ∈ {0,1}).  Faces that don't implement it
     *  simply keep their content static. */
    void (*aod_pixel_shift)(int dx, int dy);
} watch_face_t;

/* ── Registry ──────────────────────────────────────────────────────── */

/** Register the built-in faces. Call once during UI init. */
void watch_face_registry_init(void);

/** Register an additional face (max WATCH_FACE_MAX). */
void watch_face_register(const watch_face_t *face);

/** Number of registered faces. */
uint8_t watch_face_count(void);

/** Face by array index (0..count-1), or NULL. */
const watch_face_t *watch_face_get(uint8_t index);

/** Face by id, or NULL. */
const watch_face_t *watch_face_get_by_id(uint8_t id);

/** Currently active face, or NULL before any is created. */
const watch_face_t *watch_face_current(void);

/** Screen object of the current face, or NULL. */
lv_obj_t *watch_face_current_screen(void);

/** id of the current face. */
uint8_t watch_face_current_id(void);

/** Create (but do NOT load) the face screen for @p id.
 *  Used at boot so the boot-up screen stays visible while the face is
 *  prepared off-screen. Unknown ids fall back to face 0.
 *  @return true on success. */
bool watch_face_create_for_boot(uint8_t id);

/** Activate a face by id: destroys the current face screen, creates the
 *  new one and loads it as the active screen.
 *  @return false if the id is unknown / creation failed. */
bool watch_face_activate(uint8_t id);

/* ── Dispatch helpers (no-op when no face is active) ───────────────── */

/** Forward formatted time/date to the current face. */
void watch_face_update_current_time(const char *time_str, const char *date_str);

/** Forward battery state to the current face. */
void watch_face_update_current_battery(int pct, bool charging, bool vbus_present);

/** Forward an AOD pixel-shift (dx, dy) to the current face. No-op when the
 *  face doesn't implement it. Call under the LVGL lock while AOD is active. */
void watch_face_aod_pixel_shift(int dx, int dy);

#ifdef __cplusplus
}
#endif
