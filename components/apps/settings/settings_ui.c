/**
 * @file settings_ui.c
 * @brief Settings app coordinator.
 *
 * Owns the page tree, navigation and the public app API.  The actual
 * page machinery (screen, floating header, slide transitions, scroll
 * effect, list lifecycle) lives in the reusable sw_subpage system widget;
 * all row rendering lives in the topic modules (display / sound / network
 * / power / storage / appview / general) which build on the per-type
 * system widgets (sw_icon_btn, sw_toggle, sw_picker, ...).
 */

#include "settings_ui.h"
#include "settings_ui_internal.h"
#include "settings_config.h"
#include "sw_subpage.h"
#include "sw_icon_btn.h"
#include "sw_toggle.h"
#include "sw_entry.h"
#include "sw_comment.h"
#include "hw_rtc.h"
#include "app_config.h"
#include "app_events.h"
#include "app_lang.h"
#include "keyboard.h"
#include "gesture_recorder.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SETTINGS";

/* ====================================================================
 * Parent page for back-navigation
 * ==================================================================== */
static const page_t _page_parent[P_COUNT] = {
    [P_MAIN]    = P_MAIN,
    [P_WIFI]    = P_MAIN,
    [P_BT]      = P_MAIN,
    [P_DISP]    = P_MAIN,
    [P_DIM]     = P_DISP,
    [P_NOTIF]   = P_MAIN,
    [P_SOUND]   = P_MAIN,
    [P_HAPTICS] = P_SOUND,
    [P_PWD]     = P_MAIN,
    [P_APPS]    = P_MAIN,
    [P_APP_INFO]= P_APPS,
    [P_APPVIEW] = P_MAIN,
    [P_TIME_REGION] = P_MAIN,
    [P_REGION]  = P_TIME_REGION,
    [P_NTP]     = P_TIME_REGION,
    [P_GENERAL] = P_MAIN,
    [P_SYSTEM]  = P_MAIN,
    [P_BOOT]      = P_SYSTEM,
    [P_UPDATE]    = P_SYSTEM,
    [P_CONFIGURE] = P_SYSTEM,
    [P_OTA]     = P_SYSTEM,
    [P_LANG]    = P_GENERAL,
    [P_DEVINFO] = P_SYSTEM,
    [P_CREDITS] = P_SYSTEM,
    [P_EXPERIMENTAL] = P_GENERAL,
    [P_DEBUG]   = P_GENERAL,
    [P_POWER]       = P_MAIN,
    [P_POWER_CHIPS] = P_POWER,
    [P_POWER_CPU]   = P_POWER,
    [P_BATTERY_PAGE] = P_POWER,
    [P_SCREEN_TIME]  = P_POWER,
    [P_STORAGE]     = P_MAIN,
    [P_STORAGE_SD]     = P_STORAGE,
    [P_STORAGE_GDRIVE] = P_STORAGE,
    [P_GESTURE_REC]    = P_EXPERIMENTAL,
    [P_ACCESSIBILITY]  = P_MAIN,
    [P_VISIBILITY]     = P_ACCESSIBILITY,
    [P_GESTURE]        = P_ACCESSIBILITY,
    [P_ICONCOUNT]   = P_APPVIEW,
};

/* ====================================================================
 * Global context — one subpage engine (screen/header/list) + page state.
 * ==================================================================== */
typedef struct {
    sw_subpage_t  *sp;            /* subpage engine (screen+header+list) */
    lv_obj_t      *list;          /* current page's list container */
    lv_timer_t    *clock_timer;   /* header clock update timer */
    lv_timer_t    *live_timer;    /* live-value refresh timer */
    page_t         page;
    bool           active;
} ctx_t;
static ctx_t *G;

/* Cached scroll position per page (restored when navigating back). */
static int32_t s_page_scroll[P_COUNT];

/* Control-center deep-link state: when launched from the control center,
 * the target page is the "root" — backing out of it restores the control
 * center screen instead of navigating the normal page tree. */
static bool    s_from_cc  = false;
static page_t  s_cc_root  = P_MAIN;

/* ── Forward declarations ──────────────────────────────────────────── */
static void _go(page_t p);
static void _build_list_content(page_t p);

/* Slow periodic refresh of live values on the visible page.  Runs on the
 * LVGL task (like the clock timer); guarded so it only touches the page
 * that is currently shown and built. */
static void _live_update_cb(lv_timer_t *t)
{
    (void)t;
    if (!G || !G->active) return;
    page_t p = G->page;
    if (p == P_POWER || p == P_BATTERY_PAGE || p == P_SCREEN_TIME) {
        settings_power_live_update(p);
    }
}

/* ====================================================================
 * Header clock (UTC → local via configurable timezone offset)
 * ==================================================================== */
static void _clock_update_cb(lv_timer_t *t)
{
    lv_obj_t *label = lv_timer_get_user_data(t);
    if (!G || !label || !lv_obj_is_valid(label)) return;

    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return;

    time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
    hw_rtc_time_t local;
    hw_rtc_from_unix(ts, &local);

    lv_label_set_text_fmt(label, "%02d:%02d", local.hour, local.min);

    /* Reschedule for the next minute boundary (based on raw RTC seconds). */
    uint32_t next_ms = (uint32_t)(60 - rt.sec) * 1000;
    if (next_ms < 1000) next_ms = 60000;
    lv_timer_set_period(t, next_ms);
}

/* ====================================================================
 * Header
 * ==================================================================== */
static const char *_page_title(page_t p)
{
    switch (p) {
    case P_MAIN:     return app_lang_get("settings.title");
    case P_WIFI:     return app_lang_get("wifi.title");
    case P_BT:       return app_lang_get("bluetooth.title");
    case P_DISP:     return app_lang_get("display.title");
    case P_DIM:      return app_lang_get("display.dim_timeout");
    case P_NOTIF:    return app_lang_get("notifications.title");
    case P_SOUND:    return app_lang_get("sound.title");
    case P_HAPTICS:  return app_lang_get("sound.haptics");
    case P_PWD:      return app_lang_get("password.title");
    case P_APPS:     return app_lang_get("apps.title");
    case P_APP_INFO: return settings_apps_info_name();
    case P_APPVIEW:  return app_lang_get("app_view.title");
    case P_TIME_REGION: return app_lang_get("time_region.title");
    case P_REGION:   return app_lang_get("region.title");
    case P_NTP:      return app_lang_get("ntp.title");
    case P_GENERAL:  return app_lang_get("general.title");
    case P_SYSTEM:   return "System";
    case P_BOOT:      return "Boot";
    case P_UPDATE:    return "Update";
    case P_CONFIGURE: return "Configure";
    case P_OTA:      return "OTA Update";
    case P_LANG:     return app_lang_get("language.title");
    case P_DEVINFO:  return app_lang_get("device_info.title");
    case P_CREDITS:  return "JS Engine";
    case P_EXPERIMENTAL: return app_lang_get("experimental.title");
    case P_GESTURE_REC:  return "Gesture Recorder";
    case P_ACCESSIBILITY: return "Accessibility";
    case P_VISIBILITY:    return "Visibility";
    case P_GESTURE:       return "Gesture";
    case P_POWER:       return app_lang_get("power.title");
    case P_POWER_CHIPS: return app_lang_get("power.power_chips");
    case P_POWER_CPU:   return app_lang_get("power.cpu");
    case P_DEBUG:       return app_lang_get("debug.title");
    case P_BATTERY_PAGE: return app_lang_get("settings.battery");
    case P_SCREEN_TIME:  return "Screen Time";
    case P_ICONCOUNT:    return app_lang_get("settings.title");
    case P_STORAGE:      return app_lang_get("settings.storage");
    case P_STORAGE_SD:   return app_lang_get("settings.sd_card");
    case P_STORAGE_GDRIVE: return app_lang_get("settings.google_drive");
    default:           return app_lang_get("settings.title");
    }
}

/* Header back button handler supplied to the subpage engine. */
static void _subpage_back(void *ud)
{
    (void)ud;
    app_events_request_vibe_click();
    settings_ui_handle_back();
}

/* ====================================================================
 * List content
 * ==================================================================== */
static void _build_main_page(void)
{
    lv_obj_t *list = settings_ui_list();
    for (int i = 0; i < settings_main_rows_count; i++) {
        const settings_main_row_t *r = &settings_main_rows[i];
        const char *txt = r->label_key ? app_lang_get(r->label_key)
                                       : r->literal;
        sw_icon_btn_add(list, lv_color_hex(r->color), r->icon,
                        txt, settings_ui_go_nav_cb,
                        (void *)(uintptr_t)r->target);
    }
}

static void _build_list_content(page_t p)
{
    switch (p) {
    case P_MAIN:  _build_main_page();  break;
    case P_NOTIF: sw_comment_add(settings_ui_list(),
                                 app_lang_get("notifications.coming_soon"));
                  break;
    case P_APPS:  case P_APP_INFO:
        settings_apps_build(p);
        break;
    case P_PWD: {
        lv_obj_t *list = settings_ui_list();
        sw_toggle_add(list, "Enable Password", false, NULL, NULL);
        sw_entry_add(list, "Change Password",
                     settings_ui_go_nav_cb, (void *)(uintptr_t)P_MAIN);
        sw_toggle_add(list, "Simple Password", false, NULL, NULL);
        sw_comment_add(list, "Use 4-digit code instead of 6-digit");
        break;
    }
    case P_WIFI:  case P_BT:
        settings_network_build(p);
        break;
    case P_DISP:  case P_DIM:
        settings_display_build(p);
        break;
    case P_SOUND: case P_HAPTICS:
        settings_sound_build(p);
        break;
    case P_POWER: case P_POWER_CHIPS: case P_POWER_CPU:
    case P_BATTERY_PAGE: case P_SCREEN_TIME:
        settings_power_build(p);
        break;
    case P_STORAGE: case P_STORAGE_SD: case P_STORAGE_GDRIVE:
        settings_storage_build(p);
        break;
    case P_APPVIEW: case P_ICONCOUNT:
        settings_appview_build(p);
        break;
    case P_GENERAL: case P_LANG: case P_SYSTEM: case P_BOOT: case P_UPDATE:
    case P_CONFIGURE: case P_OTA: case P_DEVINFO:
    case P_CREDITS: case P_EXPERIMENTAL: case P_DEBUG: case P_GESTURE_REC:
    case P_ACCESSIBILITY: case P_VISIBILITY: case P_GESTURE:
    case P_TIME_REGION: case P_REGION: case P_NTP:
        settings_general_build(p);
        break;
    default:
        break;
    }
}

/* The subpage engine calls this to fill the list of the current page. */
static void _dispatch_build(lv_obj_t *list, void *ud)
{
    page_t p = (page_t)(uintptr_t)ud;
    G->list = list;
    _build_list_content(p);
}

/* ====================================================================
 * _go — Apple Watch slide via the sw_subpage engine.
 * ==================================================================== */
static void _go(page_t p)
{
    if (!G || !G->active || !G->sp) return;

    page_t old_page = G->page;

    /* Page-leave hooks (stop page timers, deinit WiFi, etc.). */
    settings_page_leave(old_page);

    /* Save current page's scroll position before leaving it. */
    if (G->list && lv_obj_is_valid(G->list)) {
        s_page_scroll[old_page] = lv_obj_get_scroll_y(G->list);
    }

    bool back = (old_page != P_MAIN && p == _page_parent[old_page]);

    /* Build + show the new page (screen, header title, list rows). */
    G->page = p;
    sw_subpage_show(G->sp, _page_title(p), _dispatch_build,
                    (void *)(uintptr_t)p, back);
    G->list = sw_subpage_list(G->sp);

    /* Page-enter hooks (start page timers, kick off scans, etc.). */
    settings_page_enter(p);

    /* Restore the target page's cached scroll position. */
    int32_t target_scroll = s_page_scroll[p];
    if (target_scroll > 0 && G->list) {
        lv_obj_update_layout(G->list);
        int32_t max_scroll = lv_obj_get_scroll_bottom(G->list);
        if (target_scroll > max_scroll) target_scroll = max_scroll;
        lv_obj_scroll_to_y(G->list, target_scroll, LV_ANIM_OFF);
    }
}

/* ====================================================================
 * Internal accessors for the topic modules
 * ==================================================================== */
lv_obj_t *settings_ui_list(void)          { return G ? G->list : NULL; }
void      settings_ui_refresh(void)       { if (G && G->sp) sw_subpage_refresh(G->sp); }
void      settings_ui_go(page_t p)        { _go(p); }
page_t    settings_ui_current_page(void)  { return G ? G->page : P_MAIN; }
page_t    settings_ui_parent_page(page_t p) { return _page_parent[p]; }
bool      settings_ui_is_active(void)     { return G && G->active; }

void settings_ui_header_set_title(const char *title)
{
    if (G && G->sp) sw_subpage_set_title(G->sp, title);
}

void settings_ui_vibe(void)
{
    app_events_request_vibe_click();
}

void settings_ui_go_nav_cb(lv_event_t *e)
{
    app_events_request_vibe_click();
    _go((page_t)(uintptr_t)lv_event_get_user_data(e));
}

/* ── Lifecycle dispatch ────────────────────────────────────────────── */
void settings_page_enter(page_t p)
{
    if (p == P_WIFI) settings_network_enter();
    if (p == P_OTA || p == P_GESTURE_REC) settings_general_enter();
}

void settings_page_leave(page_t p)
{
    if (p == P_WIFI || p == P_BT) settings_network_leave();
    if (p == P_OTA || p == P_GESTURE_REC) settings_general_leave();
}

/* ====================================================================
 * Public API
 * ==================================================================== */

void settings_ui_set_header_visible(bool visible)
{
    if (G && G->sp) sw_subpage_set_header_visible(G->sp, visible);
}

void settings_ui_handle_back(void)
{
    if (!G || !G->active) return;

    /* A full-screen overlay (keyboard) is showing — close it first. */
    if (keyboard_is_active()) {
        keyboard_close();
        return;
    }

    if (s_from_cc && G->page == s_cc_root) {
        /* Backing out of the deep-linked page returns to the control
         * center (settings_ui_destroy restores the CC screen). */
        ESP_LOGI(TAG, "Back at CC root → return to control center");
        g_settings_return_to_cc = true;
        settings_ui_destroy();
        return;
    }

    if (G->page == P_MAIN) {
        ESP_LOGI(TAG, "Back at main → exit settings");
        settings_ui_destroy();
    } else {
        page_t parent = _page_parent[G->page];
        ESP_LOGI(TAG, "Back from sub-page → parent");
        _go(parent);
    }
}

void settings_ui_create(void)
{
    if (G) { ESP_LOGW(TAG, "Already active"); return; }
    G = calloc(1, sizeof(ctx_t));
    if (!G) return;

    G->active = true;

    /* ── Apply saved debug states ────────────────────────────────── */
    if (!g_config.log_enabled) {
        esp_log_level_set("*", ESP_LOG_NONE);
    }
    if (g_config.cpu_monitor_enabled) {
        lv_sysmon_show_performance(NULL);
    } else {
        lv_sysmon_hide_performance(NULL);
    }

    /* Subpage engine — captures the watch-face screen to restore later. */
    G->sp = sw_subpage_create(_page_title(P_MAIN), _subpage_back, NULL);
    if (!G->sp) {
        free(G);
        G = NULL;
        return;
    }

    /* Header clock — one timer writing into the subpage's clock slot. */
    G->clock_timer = lv_timer_create(_clock_update_cb, 100,
                                     sw_subpage_clock_label(G->sp));
    lv_timer_set_repeat_count(G->clock_timer, -1);
    _clock_update_cb(G->clock_timer);

    /* Slow live-refresh timer — keeps dynamic values (battery %, CPU/PMIC
     * temperatures, screen-time numbers) current while their page is shown. */
    G->live_timer = lv_timer_create(_live_update_cb, 2000, NULL);
    lv_timer_set_repeat_count(G->live_timer, -1);

    /* Instant load (no animation) — the SH8601 SPI queue overflows on the
     * first animated frame right after the launcher is destroyed. */
    G->page = P_MAIN;
    sw_subpage_show_instant(G->sp, _page_title(P_MAIN), _dispatch_build,
                            (void *)(uintptr_t)P_MAIN);
    G->list = sw_subpage_list(G->sp);
    ESP_LOGI(TAG, "Created");
}

static page_t _cc_target_page(settings_cc_target_t t)
{
    switch (t) {
    case SETTINGS_CC_DISPLAY:    return P_DISP;
    case SETTINGS_CC_BRIGHTNESS: return P_DIM;
    case SETTINGS_CC_VOLUME:     return P_SOUND;
    case SETTINGS_CC_BATTERY:    return P_BATTERY_PAGE;
    default:                     return P_MAIN;
    }
}

void settings_ui_launch_from_cc(settings_cc_target_t target)
{
    page_t p = _cc_target_page(target);

    if (G) {
        /* Settings is ALREADY open — the CC was opened over the running
         * Settings app and the shell has already dropped the CC.  Jump to
         * the requested page inside the existing settings.  It keeps NORMAL
         * back behaviour (NOT return-to-CC) because it was not launched
         * from the CC.  If it is already showing the target page there is
         * nothing to do (avoids a pointless self-slide). */
        if (G->active && p != P_MAIN && p != G->page) {
            _go(p);
            ESP_LOGI(TAG, "Settings already open → jump to page %d", (int)p);
        }
        return;
    }

    s_from_cc = true;
    s_cc_root = p;

    settings_ui_create();
    if (!G) {
        s_from_cc = false;
        s_cc_root = P_MAIN;
        return;
    }

    /* Deep-link straight to the target page (slides in over the CC screen,
     * which stays alive underneath as the subpage's previous screen). */
    if (s_cc_root != P_MAIN) {
        _go(s_cc_root);
    }
    ESP_LOGI(TAG, "Launched from control center → page %d", (int)s_cc_root);
}

void settings_ui_destroy(void)
{
    if (!G) return;
    G->active = false;

    /* If the keyboard is still open, close it first. */
    if (keyboard_is_active()) {
        keyboard_close();
    }

    /* Stop any page timers (wifi / ota / gesture). */
    settings_page_leave(G->page);

    /* Delete the clock timer FIRST — its user_data points to the header
     * clock label that the subpage is about to delete. */
    if (G->clock_timer) { lv_timer_del(G->clock_timer); G->clock_timer = NULL; }
    if (G->live_timer)  { lv_timer_del(G->live_timer);  G->live_timer  = NULL; }

    /* Stop any in-progress IMU recording when leaving Settings. */
    if (gesture_recorder_is_recording()) {
        gesture_recorder_stop();
    }

    /* Teardown the subpage: stops effects, deletes the floating header and
     * restores the watch face (auto_del deletes the page screen). */
    if (G->sp) {
        sw_subpage_destroy(G->sp);
        G->sp = NULL;
    }
    G->list = NULL;

    free(G);
    G = NULL;

    /* Reset the control-center deep-link state. */
    s_from_cc = false;
    s_cc_root = P_MAIN;

    ESP_LOGI(TAG, "Destroyed");
}
