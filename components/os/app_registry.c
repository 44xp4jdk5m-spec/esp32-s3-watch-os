/**
 * @file app_registry.c
 * @brief Unified app registry (see app_registry.h).
 *
 * Native apps use their real launcher PNG icons (from png_images_circle.h);
 * JS apps are appended live from js_app_registry.  Launching only sets the
 * matching shell pending flag — the shell performs the screen transition.
 */

#include "app_registry.h"
#include "png_images_circle.h"
#include "js_app_registry.h"
#include "app_events.h"

/* ── Native (compiled-in) apps ─────────────────────────────────────── */
static const app_entry_t s_native_apps[] = {
    { "settings",   "Settings",   &settings_96,   0x2094FA,
      false, -1, APP_LAUNCH_SETTINGS },
    { "photos",     "Photos",     &photos_96,     0x5AC8FA,
      false, -1, APP_LAUNCH_PHOTOS },
    { "calculator", "Calculator", &calculator_96, 0xFF9500,
      false, -1, APP_LAUNCH_CALCULATOR },
    { "noise",      "Noise",      &Noise_96,      0xFA114F,
      false, -1, APP_LAUNCH_NOISE },
    { "activity",   "Activity",   &Activity_96,   0x04DE71,
      false, -1, APP_LAUNCH_ACTIVITY },
};
#define APP_NATIVE_COUNT  (sizeof(s_native_apps) / sizeof(s_native_apps[0]))

int app_registry_count(void)
{
    return (int)APP_NATIVE_COUNT + js_app_registry_count();
}

const app_entry_t *app_registry_get(int index)
{
    if (index >= 0 && index < (int)APP_NATIVE_COUNT) {
        return &s_native_apps[index];
    }

    /* JS app — returned from an internal static buffer (callers use it
     * immediately, e.g. to build one list row). */
    int j = index - (int)APP_NATIVE_COUNT;
    const js_app_entry_t *je = js_app_registry_get(j);
    if (!je) return NULL;

    static app_entry_t js;
    js.id       = je->name;
    js.name     = je->name;
    js.icon     = je->icon;
    js.color    = 0x242424;
    js.is_js    = true;
    js.js_index = j;
    js.launch   = APP_LAUNCH_SETTINGS;   /* unused for JS */
    return &js;
}

void app_registry_request_launch(int index)
{
    if (index >= 0 && index < (int)APP_NATIVE_COUNT) {
        switch (s_native_apps[index].launch) {
        case APP_LAUNCH_SETTINGS:   g_pending_settings   = true; break;
        case APP_LAUNCH_PHOTOS:     g_pending_photos     = true; break;
        case APP_LAUNCH_CALCULATOR: g_pending_calculator = true; break;
        case APP_LAUNCH_NOISE:      g_pending_noise      = true; break;
        case APP_LAUNCH_ACTIVITY:   g_pending_activity   = true; break;
        default: break;
        }
    } else {
        int j = index - (int)APP_NATIVE_COUNT;
        if (j >= 0 && j < js_app_registry_count()) {
            g_pending_jsapp = j;
        }
    }
}
