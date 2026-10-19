/**
 * @file sheet.c
 * @brief Shared frosted-glass modal core for the Action Sheet and Warning
 *        system widgets (LVGL v9).
 *
 * Both widgets are the same full-screen frosted modal with a real-time
 * backdrop blur (the notification-center technique, but hosted on
 * lv_layer_top() so it floats above every app screen including the
 * Settings header).  They differ only in their mode:
 *
 *   Action sheet (active / user-initiated):
 *     - floating cancel X (top-left) + floating live clock (top-right)
 *     - caller-chosen Material-symbol title
 *     - N frosted-grey action buttons
 *   Warning (passive):
 *     - floating live clock only (no cancel)
 *     - warning symbol + green "OK" primary + grey "Cancel" secondary
 *
 * Layout: everything except the floating top bar (cancel + time) lives in
 * ONE vertical scrollable column (title → content → actions).  A grow
 * spacer pins the actions to the bottom when the content is short and
 * collapses to zero when it is long, so the actions never overlap the
 * content and never leave an extra gap.
 *
 * A 1 s clock timer refreshes the floating time on minute boundaries and
 * resets g_last_activity_us so the display stays awake while reading.
 */

#include "actionsheet.h"
#include "warning.h"
#include "fonts.h"
#include "noto_font_24_bold.h"
#include "mi_symbols.h"
#include "app_events.h"
#include "app_lang.h"
#include "app_config.h"
#include "hw_rtc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SHEET";

#define SCR_W      410
#define SCR_H      502

#define BLUR_RADIUS   10        /* strong but not extreme, like the notification center */
#define FADE_MS       200       /* open fade duration */

/* ── Geometry ─────────────────────────────────────────────────────── */
#define TOP_BAR_H     118       /* room for the floating cancel + time */
#define CONTENT_W     345       /* content / action width (SquareLine)  */
#define ACTION_H       96
#define ACTION_RADIUS  48
#define ACTION_GAP     14       /* gap between stacked action buttons   */

/* ── Colours ──────────────────────────────────────────────────────── */
#define CLR_FROST_GREY     lv_color_hex(0x6C6C6C)   /* SquareLine grey */
#define CLR_FROST_OPA      LV_OPA_70                /* translucent → blur shows through */
#define CLR_WARN_GREEN     lv_color_hex(0x04DE71)
#define CLR_DANGER_RED     lv_color_hex(0xFF3B30)   /* destructive action */
#define CLR_TEXT           lv_color_hex(0xFFFFFF)
#define CLR_BODY           lv_color_hex(0xE4E7EC)
#define CLR_PRESSED_BOOST  56                       /* lighten amount while pressed */

/* ── Widget state (only one modal at a time) ──────────────────────── */
typedef enum { SHEET_ACTION_SHEET, SHEET_WARNING } sheet_mode_t;

typedef struct {
    sheet_mode_t     mode;
    lv_obj_t        *root;      /* full-screen container on lv_layer_top */
    lv_obj_t        *time;      /* floating live clock label */
    lv_obj_t        *cancel;    /* floating cancel X (action sheet only) */
    lv_obj_t        *scroll;    /* scrollable column: title+content+actions */
    lv_timer_t      *clock;     /* 1 s time + keep-awake timer           */
    action_sheet_cb_t cb;       /* both widgets share the same cb shape  */
    void            *ud;
    char             title_icon[8];
    char             content[512];
    char             actions[ACTION_SHEET_MAX_ACTIONS][48];
    int              action_count;
    bool             destructive_last;  /* last action drawn in danger red */
    bool             active;
} sheet_ctx_t;

static sheet_ctx_t s_ctx = { .mode = SHEET_ACTION_SHEET };

/* ── Close / teardown ─────────────────────────────────────────────── */

static void sheet_close(int result_idx)
{
    if (!s_ctx.active || !s_ctx.root) return;

    action_sheet_cb_t cb = s_ctx.cb;
    void *ud = s_ctx.ud;

    if (s_ctx.clock) { lv_timer_del(s_ctx.clock); }
    lv_obj_t *root = s_ctx.root;
    memset(&s_ctx, 0, sizeof(s_ctx));

    /* Async so it is safe to call from inside an LVGL event callback. */
    lv_obj_delete_async(root);

    ESP_LOGI(TAG, "closed (idx=%d)", result_idx);
    if (cb) cb(result_idx, ud);
}

static void sheet_cancel_cb(lv_event_t *e)
{
    (void)e;
    app_events_request_vibe_click();
    sheet_close(-1);
}

static void sheet_action_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_events_request_vibe_click();
    sheet_close(idx);
}

/* ── Floating clock + keep-awake ──────────────────────────────────── */

static void sheet_clock_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_ctx.active || !s_ctx.time || !lv_obj_is_valid(s_ctx.time)) return;

    /* Keep the display awake while the modal is open (inactivity timer). */
    g_last_activity_us = esp_timer_get_time();

    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return;

    time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
    hw_rtc_time_t local;
    hw_rtc_from_unix(ts, &local);

    static char last[8] = "";
    char now[8];
    snprintf(now, sizeof(now), "%02d:%02d", local.hour, local.min);
    if (strcmp(now, last) != 0) {
        strncpy(last, now, sizeof(last) - 1);
        lv_label_set_text(s_ctx.time, now);
    }
}

/* ── Frosted button (translucent so the blur shows through) ───────── */

static lv_obj_t *sheet_make_button(lv_color_t color, lv_event_cb_t cb,
                                   void *ud, int h)
{
    lv_obj_t *btn = lv_button_create(s_ctx.scroll);
    lv_obj_set_width(btn, CONTENT_W);
    lv_obj_set_height(btn, h);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, ACTION_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, CLR_FROST_OPA, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    /* Brighter + fully opaque while pressed. */
    lv_obj_set_style_bg_color(btn, lv_color_lighten(color, CLR_PRESSED_BOOST), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    return btn;
}

/* ── Scrollable column: title symbol + content + actions ──────────── */

static void sheet_build_content(void)
{
    if (!s_ctx.scroll) return;

    /* Title — Material symbol scaled ~2× (256 = 100 %, so 512 = 200 %). */
    lv_obj_t *title = lv_label_create(s_ctx.scroll);
    lv_label_set_text(title, s_ctx.title_icon[0] ? s_ctx.title_icon : MI_INFO);
    lv_obj_set_style_text_font(title, &mi_font_24, 0);
    lv_obj_set_style_text_color(title, CLR_TEXT, 0);
    lv_obj_set_style_transform_scale(title, 512, 0);
    lv_obj_set_style_transform_pivot_x(title, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(title, lv_pct(50), 0);
    lv_obj_set_style_margin_top(title, 8, 0);
    lv_obj_set_style_margin_bottom(title, 6, 0);

    /* Body — centered, wraps within CONTENT_W. */
    lv_obj_t *body = lv_label_create(s_ctx.scroll);
    lv_label_set_text(body, s_ctx.content);
    lv_obj_set_width(body, CONTENT_W);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body, &noto_font_24, 0);
    lv_obj_set_style_text_color(body, CLR_BODY, 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(body, 6, 0);
    lv_obj_set_style_margin_bottom(body, 14, 0);

    /* Grow spacer — pins the actions to the bottom when content is short,
     * collapses to zero when the column overflows (long content), so the
     * actions never overlap the content and never leave an extra gap. */
    lv_obj_t *spacer = lv_obj_create(s_ctx.scroll);
    lv_obj_remove_style_all(spacer);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    /* Action buttons (warning: green primary, grey secondary; action
     * sheet: frosted grey, except the last one when destructive_last). */
    for (int i = 0; i < s_ctx.action_count; i++) {
        lv_color_t color = CLR_FROST_GREY;
        if (s_ctx.mode == SHEET_WARNING) {
            color = (i == 0) ? CLR_WARN_GREEN : CLR_FROST_GREY;
        } else if (s_ctx.destructive_last && i == s_ctx.action_count - 1) {
            color = CLR_DANGER_RED;
        }
        lv_obj_t *btn = sheet_make_button(color, sheet_action_cb,
                                          (void *)(intptr_t)i, ACTION_H);
        if (i < s_ctx.action_count - 1) {
            lv_obj_set_style_margin_bottom(btn, ACTION_GAP, 0);
        }
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_ctx.actions[i]);
        lv_obj_set_style_text_font(lbl, &noto_font_24, 0);
        lv_obj_set_style_text_color(lbl, CLR_TEXT, 0);
        lv_obj_center(lbl);
    }
}

/* ── Full-screen frosted modal ────────────────────────────────────── */

static void sheet_build(void)
{
    /* Root: full-screen clickable container on the top layer.  Clickable
     * so touches are blocked from reaching the (blurred) UI beneath.  The
     * real-time backdrop blur lives on the root itself. */
    s_ctx.root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_ctx.root, SCR_W, SCR_H);
    lv_obj_set_pos(s_ctx.root, 0, 0);
    lv_obj_remove_flag(s_ctx.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ctx.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_ctx.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ctx.root, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.root, 0, 0);
    lv_obj_set_style_radius(s_ctx.root, 0, 0);
    lv_obj_set_style_blur_radius(s_ctx.root, BLUR_RADIUS, 0);
    lv_obj_set_style_blur_backdrop(s_ctx.root, true, 0);
    lv_obj_set_style_blur_quality(s_ctx.root, LV_BLUR_QUALITY_SPEED, 0);

    /* Floating live clock (top-right) — floats above the scroll area. */
    s_ctx.time = lv_label_create(s_ctx.root);
    lv_obj_set_style_text_font(s_ctx.time, &noto_font_24_bold, 0);
    lv_obj_set_style_text_color(s_ctx.time, CLR_TEXT, 0);
    lv_obj_align(s_ctx.time, LV_ALIGN_TOP_RIGHT, -30, 30);
    lv_label_set_text(s_ctx.time, "--:--");

    /* Floating cancel X (action sheet only). */
    if (s_ctx.mode == SHEET_ACTION_SHEET) {
        s_ctx.cancel = lv_button_create(s_ctx.root);
        lv_obj_set_size(s_ctx.cancel, 71, 71);
        lv_obj_align(s_ctx.cancel, LV_ALIGN_TOP_LEFT, 32, 39);
        lv_obj_remove_flag(s_ctx.cancel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_ctx.cancel, 50, 0);
        lv_obj_set_style_bg_color(s_ctx.cancel, CLR_FROST_GREY, 0);
        lv_obj_set_style_bg_opa(s_ctx.cancel, CLR_FROST_OPA, 0);
        lv_obj_set_style_bg_color(s_ctx.cancel,
            lv_color_lighten(CLR_FROST_GREY, CLR_PRESSED_BOOST), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_ctx.cancel, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(s_ctx.cancel, 0, 0);
        lv_obj_set_style_shadow_width(s_ctx.cancel, 0, 0);
        lv_obj_add_event_cb(s_ctx.cancel, sheet_cancel_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *xl = lv_label_create(s_ctx.cancel);
        lv_label_set_text(xl, MI_CLOSE);
        lv_obj_set_style_text_font(xl, &mi_font_24, 0);
        lv_obj_set_style_text_color(xl, CLR_TEXT, 0);
        lv_obj_center(xl);
    }

    /* Scrollable column below the floating top bar. */
    s_ctx.scroll = lv_obj_create(s_ctx.root);
    lv_obj_remove_style_all(s_ctx.scroll);
    lv_obj_set_size(s_ctx.scroll, SCR_W, SCR_H - TOP_BAR_H);
    lv_obj_set_pos(s_ctx.scroll, 0, TOP_BAR_H);
    lv_obj_set_scroll_dir(s_ctx.scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ctx.scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_ctx.scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ctx.scroll, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(s_ctx.scroll, 12, 0);
    lv_obj_set_style_pad_bottom(s_ctx.scroll, 20, 0);
    lv_obj_set_style_pad_hor(s_ctx.scroll, 0, 0);
    lv_obj_set_style_bg_opa(s_ctx.scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ctx.scroll, 0, 0);
    lv_obj_set_style_radius(s_ctx.scroll, 0, 0);

    sheet_build_content();

    lv_obj_fade_in(s_ctx.root, FADE_MS, 0);
}

/* ====================================================================
 * Public API — Action Sheet
 * ==================================================================== */

void action_sheet_open_ex(const char *title_icon,
                          const char *content,
                          const char *const *actions,
                          int action_count,
                          bool destructive_last,
                          action_sheet_cb_t cb,
                          void *user_data)
{
    if (s_ctx.active) sheet_close(-1);   /* close any open modal first */

    if (action_count < 0) action_count = 0;
    if (action_count > ACTION_SHEET_MAX_ACTIONS) action_count = ACTION_SHEET_MAX_ACTIONS;

    s_ctx.mode             = SHEET_ACTION_SHEET;
    s_ctx.cb               = cb;
    s_ctx.ud               = user_data;
    s_ctx.action_count     = action_count;
    s_ctx.destructive_last = destructive_last;
    strncpy(s_ctx.title_icon, title_icon ? title_icon : MI_INFO,
            sizeof(s_ctx.title_icon) - 1);
    s_ctx.title_icon[sizeof(s_ctx.title_icon) - 1] = '\0';
    strncpy(s_ctx.content, content ? content : "", sizeof(s_ctx.content) - 1);
    s_ctx.content[sizeof(s_ctx.content) - 1] = '\0';
    for (int i = 0; i < action_count && actions && actions[i]; i++) {
        strncpy(s_ctx.actions[i], actions[i], sizeof(s_ctx.actions[i]) - 1);
        s_ctx.actions[i][sizeof(s_ctx.actions[i]) - 1] = '\0';
    }
    s_ctx.active = true;

    sheet_build();
    s_ctx.clock = lv_timer_create(sheet_clock_cb, 1000, NULL);
    sheet_clock_cb(s_ctx.clock);
    ESP_LOGI(TAG, "action sheet open (%d actions)", action_count);
}

void action_sheet_open(const char *title_icon,
                       const char *content,
                       const char *const *actions,
                       int action_count,
                       action_sheet_cb_t cb,
                       void *user_data)
{
    action_sheet_open_ex(title_icon, content, actions, action_count,
                         false, cb, user_data);
}

void action_sheet_close(void)
{
    sheet_close(-1);
}

bool action_sheet_is_open(void)
{
    return s_ctx.active && s_ctx.mode == SHEET_ACTION_SHEET;
}

/* ====================================================================
 * Public API — Warning
 * ==================================================================== */

void warning_show(const char *content, warning_cb_t cb, void *user_data)
{
    if (s_ctx.active) sheet_close(1);

    s_ctx.mode             = SHEET_WARNING;
    s_ctx.cb               = (action_sheet_cb_t)cb;
    s_ctx.ud               = user_data;
    s_ctx.action_count     = 2;
    s_ctx.destructive_last = false;
    strncpy(s_ctx.title_icon, MI_WARNING, sizeof(s_ctx.title_icon) - 1);
    s_ctx.title_icon[sizeof(s_ctx.title_icon) - 1] = '\0';
    strncpy(s_ctx.content, content ? content : "", sizeof(s_ctx.content) - 1);
    s_ctx.content[sizeof(s_ctx.content) - 1] = '\0';
    strncpy(s_ctx.actions[0], app_lang_get("common.ok"),
            sizeof(s_ctx.actions[0]) - 1);
    s_ctx.actions[0][sizeof(s_ctx.actions[0]) - 1] = '\0';
    strncpy(s_ctx.actions[1], app_lang_get("common.cancel"),
            sizeof(s_ctx.actions[1]) - 1);
    s_ctx.actions[1][sizeof(s_ctx.actions[1]) - 1] = '\0';
    s_ctx.active = true;

    sheet_build();
    s_ctx.clock = lv_timer_create(sheet_clock_cb, 1000, NULL);
    sheet_clock_cb(s_ctx.clock);
    ESP_LOGI(TAG, "warning open");
}

void warning_hide(void)
{
    sheet_close(1);
}

bool warning_is_active(void)
{
    return s_ctx.active && s_ctx.mode == SHEET_WARNING;
}
