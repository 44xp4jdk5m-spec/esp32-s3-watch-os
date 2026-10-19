/**
 * @file settings_ui_internal.h
 * @brief Internal shared interface between the settings coordinator
 *        (settings_ui.c) and the per-topic page modules.
 *
 * The coordinator owns the page enum, the current list/header, navigation
 * and refresh; each topic module (display / sound / network / power /
 * storage / app-view / general) builds list content for its pages and may
 * hook page enter/leave to manage its own timers.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Page tree ─────────────────────────────────────────────────────── */
typedef enum {
    P_MAIN, P_WIFI, P_BT, P_DISP, P_DIM,
    P_NOTIF, P_SOUND, P_HAPTICS, P_PWD, P_APPS, P_APP_INFO, P_APPVIEW,
    P_TIME_REGION, P_REGION, P_NTP,
    P_GENERAL, P_SYSTEM, P_BOOT, P_UPDATE, P_CONFIGURE, P_OTA, P_LANG,
    P_DEVINFO, P_EXPERIMENTAL, P_DEBUG,
    P_POWER, P_POWER_CHIPS, P_POWER_CPU,
    P_BATTERY_PAGE,
    P_SCREEN_TIME,
    P_STORAGE,
    P_STORAGE_SD,
    P_STORAGE_GDRIVE,
    P_GESTURE_REC,
    P_ACCESSIBILITY,
    P_VISIBILITY,
    P_GESTURE,
    P_CREDITS,
    P_ICONCOUNT,
} page_t;
#define P_COUNT  (P_ICONCOUNT + 1)

/* ── Coordinator services (implemented in settings_ui.c) ───────────── */
lv_obj_t  *settings_ui_list(void);           /* current list container  */
void       settings_ui_refresh(void);        /* in-place list rebuild   */
void       settings_ui_go(page_t p);         /* navigate (slide anim)   */
page_t     settings_ui_current_page(void);
page_t     settings_ui_parent_page(page_t p);
bool       settings_ui_is_active(void);
void       settings_ui_header_set_title(const char *title);
void       settings_ui_vibe(void);           /* haptic click feedback   */

/* Navigation click handler: reads the target page from user_data and
 * navigates (with a vibe click).  Attach to rows that go to another page. */
void settings_ui_go_nav_cb(lv_event_t *e);

/* ── Lifecycle dispatch (settings_ui.c) ────────────────────────────── */
void settings_page_enter(page_t p);
void settings_page_leave(page_t p);

/* ── Topic hooks (implemented in the topic modules) ────────────────── */
void settings_network_enter(void);
void settings_network_leave(void);
void settings_general_enter(void);
void settings_general_leave(void);

/* ── Topic page builders (implemented in the topic modules) ────────── */
void settings_display_build(page_t p);
void settings_sound_build(page_t p);
void settings_network_build(page_t p);
void settings_power_build(page_t p);

/* Refresh live values (battery %, temperatures, screen-time numbers) on the
 * currently visible power/battery/screen-time page.  Called on a slow LVGL
 * timer from the coordinator while the page is shown. */
void settings_power_live_update(page_t p);
void settings_storage_build(page_t p);
void settings_appview_build(page_t p);
void settings_general_build(page_t p);
void settings_apps_build(page_t p);

/* App info page target (settings_apps.c) — which app P_APP_INFO shows. */
void settings_apps_set_info_index(int index);
const char *settings_apps_info_name(void);

#ifdef __cplusplus
}
#endif
