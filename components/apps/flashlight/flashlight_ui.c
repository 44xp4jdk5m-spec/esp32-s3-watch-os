/**
 * @file flashlight_ui.c
 * @brief Full-screen flashlight (see flashlight_ui.h).
 *
 * Layout mirrors tmp/flashlight/ui_flashlight.c but cleaned up:
 *   - one horizontal snap-pager holding 3 full-screen colour panels
 *   - index dots bottom-centre (demo colours: active #242424)
 *   - bold black HH:MM clock top-right (matches the Settings header clock)
 *   - the clock + index fade out after 1 s without touch and reappear on
 *     touch (each touch also resets the fade timer)
 *
 * While the flashlight is on, g_last_activity_us is poked every tick so the
 * system inactivity dim/sleep never turns the "light" down.
 */

#include "flashlight_ui.h"

#include "lvgl.h"
#include "screen_metrics.h"
#include "noto_font_24_bold.h"
#include "app_events.h"
#include "app_config.h"
#include "hw_rtc.h"
#include "hw_display.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "FLASH";

/* ── Geometry (EOS display) ────────────────────────────────────────── */
#define FL_W            EOS_DISPLAY_W           /* 410 */
#define FL_H            EOS_DISPLAY_H           /* 502 */

/* ── Modes ─────────────────────────────────────────────────────────── */
#define FL_MODES        3

/* Demo panels are rounded like the physical screen glass (radius 100). */
#define FL_PANEL_RADIUS 100

/* Clock / index overlay colours. */
#define FL_DOT_ACTIVE   0x242424                /* demo: active dot  */
#define FL_DOT_IDLE     0xB0B0B0                /* demo: idle dots   */

/* Auto-hide after this much idle (ms). */
#define FL_IDLE_HIDE_MS 1000
/* Blink cadence for mode 1 (~2 Hz → toggle every 250 ms). */
#define FL_BLINK_MS     250

static bool s_active    = false;
static lv_obj_t *s_scr  = NULL;      /* screen */
static lv_obj_t *s_prev = NULL;      /* control center underneath */
static lv_obj_t *s_pager = NULL;     /* horizontal snap pager      */
static lv_obj_t *s_panel[FL_MODES];  /* colour pages               */
static lv_obj_t *s_time = NULL;      /* bold black HH:MM top-right */
static lv_obj_t *s_dots = NULL;      /* index-dot wrapper          */
static lv_obj_t *s_dot[FL_MODES];

static lv_timer_t *s_blink_timer = NULL;
static lv_timer_t *s_idle_timer  = NULL;
static lv_timer_t *s_clock_timer = NULL;

static int  s_mode    = 0;           /* current centred mode */
static bool s_ui_vis  = true;        /* time + index shown   */
static bool s_wake    = false;       /* touch seen → reshow  */
static int32_t s_idle_ms = 0;

/* ── Mode table ────────────────────────────────────────────────────── */
static const uint32_t s_color[FL_MODES] = { 0xFFFFFF, 0xFFFFFF, 0xFF0000 };
static const bool     s_blink[FL_MODES] = { false,    true,     false };

/* ── Forward decls ─────────────────────────────────────────────────── */
static void fl_set_mode(int idx);
static void fl_show_ui(void);
static void fl_hide_ui(void);

/* ====================================================================
 * UI state helpers
 * ==================================================================== */
static void fl_show_ui(void)
{
    if (s_ui_vis || !s_scr) return;
    s_ui_vis = true;
    if (s_time && lv_obj_is_valid(s_time)) lv_obj_fade_in(s_time, 200, 0);
    if (s_dots && lv_obj_is_valid(s_dots)) lv_obj_fade_in(s_dots, 200, 0);
}

static void fl_hide_ui(void)
{
    if (!s_ui_vis || !s_scr) return;
    s_ui_vis = false;
    if (s_time && lv_obj_is_valid(s_time)) lv_obj_fade_out(s_time, 300, 0);
    if (s_dots && lv_obj_is_valid(s_dots)) lv_obj_fade_out(s_dots, 300, 0);
}

/* Any touch on the screen wakes the overlay + resets the idle timer. */
static void fl_touch_cb(lv_event_t *e)
{
    (void)e;
    s_wake = true;
}

/* ====================================================================
 * Timers
 * ==================================================================== */
static void fl_blink_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;
    if (s_blink[s_mode] && s_panel[s_mode]) {
        if (lv_obj_has_flag(s_panel[s_mode], LV_OBJ_FLAG_HIDDEN))
            lv_obj_remove_flag(s_panel[s_mode], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_panel[s_mode], LV_OBJ_FLAG_HIDDEN);
    }
    /* keep the light alive — never let inactivity dim/sleep it */
    g_last_activity_us = esp_timer_get_time();
}

static void fl_idle_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) return;

    if (s_wake) {
        s_wake = false;
        s_idle_ms = 0;
        fl_show_ui();
        return;
    }

    if (s_ui_vis) {
        s_idle_ms += 100;
        if (s_idle_ms >= FL_IDLE_HIDE_MS) {
            s_idle_ms = 0;
            fl_hide_ui();
        }
    }
    g_last_activity_us = esp_timer_get_time();
}

static void fl_clock_cb(lv_timer_t *t)
{
    lv_obj_t *label = lv_timer_get_user_data(t);
    if (!s_active || !label || !lv_obj_is_valid(label)) return;

    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return;

    time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
    hw_rtc_time_t local;
    hw_rtc_from_unix(ts, &local);

    lv_label_set_text_fmt(label, "%02d:%02d", local.hour, local.min);

    /* Reschedule for the next minute boundary (raw RTC seconds). */
    uint32_t next_ms = (uint32_t)(60 - rt.sec) * 1000;
    if (next_ms < 1000) next_ms = 60000;
    lv_timer_set_period(t, next_ms);
}

/* ====================================================================
 * Mode / snap handling
 * ==================================================================== */
static void fl_set_mode(int idx)
{
    if (idx < 0) idx = 0;
    if (idx >= FL_MODES) idx = FL_MODES - 1;
    s_mode = idx;

    /* leave blink panel fully visible unless we are in blink mode */
    if (s_panel[1]) lv_obj_remove_flag(s_panel[1], LV_OBJ_FLAG_HIDDEN);

    /* index dots: active = dark, idle = light grey */
    for (int i = 0; i < FL_MODES; i++) {
        if (!s_dot[i]) continue;
        lv_obj_set_style_bg_color(s_dot[i],
                                  lv_color_hex(i == s_mode ? FL_DOT_ACTIVE
                                                           : FL_DOT_IDLE),
                                  0);
    }
}

static void fl_pager_end_cb(lv_event_t *e)
{
    lv_obj_t *pager = lv_event_get_target_obj(e);
    int x = lv_obj_get_scroll_x(pager);
    int idx = (int)floorf((x + FL_W / 2.0f) / (float)FL_W);
    if (idx < 0) idx = 0;
    if (idx >= FL_MODES) idx = FL_MODES - 1;
    fl_set_mode(idx);
    s_wake = true;                 /* snap counts as activity */
}

/* ====================================================================
 * Build
 * ==================================================================== */
static void fl_build(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, FL_W, FL_H);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Horizontal snap pager with the 3 full-screen panels ─────── */
    s_pager = lv_obj_create(s_scr);
    lv_obj_set_pos(s_pager, 0, 0);
    lv_obj_set_size(s_pager, FL_W, FL_H);
    lv_obj_set_style_pad_all(s_pager, 0, 0);
    lv_obj_set_style_border_width(s_pager, 0, 0);
    lv_obj_set_style_bg_opa(s_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_pager, 0, 0);
    lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_pager, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pager, fl_pager_end_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(s_pager, fl_touch_cb, LV_EVENT_PRESSED, NULL);

    for (int i = 0; i < FL_MODES; i++) {
        s_panel[i] = lv_obj_create(s_pager);
        lv_obj_set_pos(s_panel[i], FL_W * i, 0);
        lv_obj_set_size(s_panel[i], FL_W, FL_H);
        lv_obj_set_style_pad_all(s_panel[i], 0, 0);
        lv_obj_set_style_border_width(s_panel[i], 0, 0);
        lv_obj_set_style_radius(s_panel[i], FL_PANEL_RADIUS, 0);
        lv_obj_set_style_clip_corner(s_panel[i], true, 0);
        lv_obj_set_style_bg_color(s_panel[i], lv_color_hex(s_color[i]), 0);
        lv_obj_set_style_bg_opa(s_panel[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_panel[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_panel[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_panel[i], fl_touch_cb, LV_EVENT_PRESSED, NULL);
    }

    /* ── Bold black clock, top-right (Settings header position) ──── */
    s_time = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_time, &noto_font_24_bold, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(0x000000), 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_RIGHT, -30, 32);
    lv_label_set_text(s_time, "--:--");

    /* ── Index dots, bottom-centre (demo layout) ─────────────────── */
    s_dots = lv_obj_create(s_scr);
    lv_obj_set_size(s_dots, FL_W, 10);
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dots, 0, 0);
    lv_obj_set_style_pad_all(s_dots, 0, 0);
    for (int i = 0; i < FL_MODES; i++) {
        s_dot[i] = lv_obj_create(s_dots);
        lv_obj_set_size(s_dot[i], 10, 10);
        lv_obj_remove_flag(s_dot[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_dot[i], 0, 0);
        lv_obj_align(s_dot[i], LV_ALIGN_CENTER, (i - 1) * 18, 0);
    }
    fl_set_mode(0);   /* dot colours + blink panel state */

    lv_screen_load(s_scr);
}

/* ====================================================================
 * Public API
 * ==================================================================== */
void flashlight_ui_init(void)
{
    ESP_LOGI(TAG, "Flashlight ready");
}

bool flashlight_ui_is_active(void)
{
    return s_active;
}

void flashlight_ui_open(void)
{
    if (s_active) return;

    s_prev = lv_screen_active();
    if (!s_prev) return;

    s_idle_ms = 0;
    s_wake = false;
    s_ui_vis = true;

    fl_build();

    s_blink_timer = lv_timer_create(fl_blink_cb, FL_BLINK_MS, NULL);
    lv_timer_set_repeat_count(s_blink_timer, -1);
    s_idle_timer = lv_timer_create(fl_idle_cb, 100, NULL);
    lv_timer_set_repeat_count(s_idle_timer, -1);
    s_clock_timer = lv_timer_create(fl_clock_cb, 100, s_time);
    lv_timer_set_repeat_count(s_clock_timer, -1);
    fl_clock_cb(s_clock_timer);

    hw_display_set_brightness(100);       /* full light */
    g_last_activity_us = esp_timer_get_time();

    s_active = true;
    ESP_LOGI(TAG, "Opened (brightness 100)");
}

void flashlight_ui_close(void)
{
    if (!s_active) return;
    s_active = false;

    if (s_blink_timer) { lv_timer_del(s_blink_timer); s_blink_timer = NULL; }
    if (s_idle_timer)  { lv_timer_del(s_idle_timer);  s_idle_timer  = NULL; }
    if (s_clock_timer) { lv_timer_del(s_clock_timer); s_clock_timer = NULL; }

    /* restore the control center underneath */
    if (s_prev && lv_obj_is_valid(s_prev)) {
        lv_screen_load(s_prev);
    }
    if (s_scr && lv_obj_is_valid(s_scr)) lv_obj_delete(s_scr);

    hw_display_set_brightness(g_config.brightness);
    g_last_activity_us = esp_timer_get_time();

    s_scr = NULL; s_prev = NULL; s_pager = NULL;
    memset(s_panel, 0, sizeof(s_panel));
    s_time = NULL; s_dots = NULL;
    memset(s_dot, 0, sizeof(s_dot));
    s_mode = 0;

    ESP_LOGI(TAG, "Closed (brightness %d)", g_config.brightness);
}

void flashlight_ui_handle_back(void)
{
    if (!s_active) return;
    app_events_request_vibe_click();
    flashlight_ui_close();
}
