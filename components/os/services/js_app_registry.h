/*
 * Watch OS — JavaScript app registry.
 *
 * Scans /sdcard/watchos/usr/app/<APP_NAME>/ for installed JS apps at boot
 * (and re-scans on demand), reads each manifest.json and decodes each
 * app_icon.bin (raw RGB565) into PSRAM via app_img_load_raw().  The apps
 * themselves never live in firmware — only their metadata + decoded icons
 * are held in RAM while the OS runs.
 */
#ifndef JS_APP_REGISTRY_H
#define JS_APP_REGISTRY_H

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct js_app_entry {
    char          *name;       /* display name (from manifest)        */
    char          *dir;        /* /sdcard/watchos/usr/app/<name>      */
    char          *entry;      /* relative entry script, e.g. main.js */
    char          *author;     /* manifest author (may be NULL)       */
    char          *version;    /* manifest version (may be NULL)      */
    char          *description;/* manifest description (may be NULL)  */
    lv_image_dsc_t *icon;      /* decoded icon in PSRAM (or NULL)     */
    uint32_t       icon_w;
    uint32_t       icon_h;
} js_app_entry_t;

/* (Re)scan the SD card app folder.  Safe to call repeatedly; frees the
 * previous result set.  Returns the number of apps found (0 if no SD or
 * no apps installed). */
int  js_app_registry_scan(void);

int  js_app_registry_count(void);
const js_app_entry_t *js_app_registry_get(int index);

/* Absolute path to the app's entry script (caller must not free). */
const char *js_app_registry_entry_path(int index);

/**
 * @brief Uninstall a JS app: recursively delete its folder from the SD
 *        card and drop it from the registry.
 * @param index  js_app_registry index (0-based)
 * @return 0 on success, -1 if the index is invalid.
 */
int  js_app_registry_delete(int index);

/**
 * @brief Clear a JS app's data folder (<app dir>/app_data) recursively.
 *        The app itself stays installed.
 * @param index  js_app_registry index (0-based)
 * @return 0 on success, -1 if the index is invalid or there is no data dir.
 */
int  js_app_registry_clear_data(int index);

/**
 * @brief Whether a JS app has data to clear: true when <app dir>/app_data
 *        exists and contains at least one file/subdirectory.
 * @param index  js_app_registry index (0-based)
 */
bool js_app_registry_has_data(int index);

#ifdef __cplusplus
}
#endif

#endif /* JS_APP_REGISTRY_H */
