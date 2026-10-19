/**
 * @file app_notification_center.c
 * @brief Notification center — real-time frosted-glass + notification list via top-edge swipe.
 *
 * Design:
 *   1. Touch within top 20px → attach an overlay to the LIVE active screen
 *   2. A frosted panel uses LVGL 9.5's native backdrop blur
 *      (LV_STYLE_BLUR_RADIUS + LV_STYLE_BLUR_BACKDROP) — whatever is on screen
 *      behind it keeps rendering and is blurred in real time (no snapshot,
 *      no manual box blur)
 *   3. As finger drags down the panel expands; release past half-screen opens it
 *   4. Notification list is a port of ElenixOS msg_list (no swipe-to-delete):
 *        - each card: app icon + title + relative time, message below
 *        - card height auto-adjusts to the message length (LV_SIZE_CONTENT + WRAP)
 *        - "Clear All" button + "No notifications" placeholder
 *        - tap a card → full message detail (blur still behind)
 *   5. Release before half-screen → collapse back to the watch face
 */

#include "app_notification_center.h"
#include "app_events.h"
#include "app_ui.h"
#include "app_notification_store.h"
#include "hw_display.h"
#include "hw_touch.h"
#include "ui.h"
#include "fonts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include "bsp/display.h"

static const char *TAG = "NCTR";

#define SCR_W         BSP_LCD_H_RES       /* 410 */
#define SCR_H         BSP_LCD_V_RES       /* 502 */
#define SWIPE_EDGE    20                  /* top 20px trigger zone */
#define OPEN_THRESHOLD (SCR_H / 2)       /* open only if pulled past half-screen */
#define MIN_SWIPE_VELOCITY 600           /* px/s required for a fling open */
#define HALF_H        (SCR_H / 2)         /* 251 — threshold for open vs dismiss */
#define MAX_DIM_OPA   LV_OPA_50           /* max dark mask = 50% */
#define DISMISS_MS    200                 /* fade-out animation duration */
#define EXPAND_MS     200                 /* panel expand/collapse duration */
#define BLUR_RADIUS   10                  /* strong blur — visible but not extreme */

typedef enum { GS_IDLE, GS_TRACKING, GS_EXPANDING, GS_OPEN, GS_COLLAPSING } gs_t;

static gs_t       gs           = GS_IDLE;
static int32_t    g_cur_y      = 0;
static int32_t    g_start_y    = 0;
static int64_t    g_start_t0   = 0;
static int64_t    g_anim_t0    = 0;
static int32_t    g_anim_opa0  = 0;
static bool       g_was_pressed = false;
static bool       g_indev_cb_added = false;
static bool       g_started_at_edge = false;

/* animation helpers for panel height animation */
static int32_t    g_anim_h0      = 0;
static int32_t    g_anim_h_target = 0;
static int32_t    g_anim_ms      = 0;

/* ── Real-time blur overlay (lives on the active screen, no snapshot) ─ */
static lv_obj_t  *g_blur_scr    = NULL;   /* overlay container on the active screen */
static lv_obj_t  *g_blur_panel  = NULL;   /* top-aligned frosted panel (native blur) */
static lv_obj_t  *g_prev_scr    = NULL;   /* screen the overlay is attached to */
static lv_obj_t  *g_last_active_scr = NULL;

/* ── Forward declaration ──────────────────────────────────────────── */
static void indev_press_cb(lv_event_t *e);

/* ── Helpers ──────────────────────────────────────────────────────── */

static void enable_bubble_recursive(lv_obj_t *obj)
{
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
        enable_bubble_recursive(child);
    }
}

/** Set the dark tint opacity on the blur panel.  The user asked for a
 * normal frosted blur with no dimming, so this is intentionally a no-op. */
static void tint_set_opa(lv_opa_t opa)
{
    (void)opa;
}

/** Map finger y-position to tint opacity: 0 at y=0, MAX_DIM_OPA at y=HALF_H. */
static lv_opa_t y_to_opa(int32_t y)
{
    if (y <= 0) return LV_OPA_0;
    if (y >= HALF_H) return MAX_DIM_OPA;
    return (lv_opa_t)((y * (int32_t)MAX_DIM_OPA) / HALF_H);
}

/* ── Notification list UI (ported from ElenixOS msg_list, no swipe) ─ */

#define NOTIF_ICON_SIZE       64
#define NOTIF_ICON_LARGE_SIZE 84
#define NOTIF_ITEM_MARGIN     25
#define NOTIF_ITEM_PAD        20
#define NOTIF_ITEM_RADIUS     30
#define NOTIF_ITEM_BG_OPA     LV_OPA_70      /* translucent → blur shows through */
#define NOTIF_LIST_PAD        30
#define NOTIF_LIST_PAD_TOP    40
#define BOTTOM_SWIPE_EDGE     32             /* bottom zone that arms swipe-up exit */
#define SWIPE_UP_MIN_DIST     60             /* upward px required to exit the center */

/* Objects owned by the blur screen (deleted with it). */
static lv_obj_t *g_notif_list       = NULL;  /* scrollable list container */
static lv_obj_t *g_notif_clear_all  = NULL;  /* clear-all button (first child) */
static lv_obj_t *g_notif_no_msg     = NULL;  /* "no notifications" label */
static lv_obj_t *g_notif_detail     = NULL;  /* full-message detail overlay */
static int32_t   g_press_x0 = 0;
static int32_t   g_press_y0 = 0;
static bool      g_bottom_swipe_armed = false; /* press started inside bottom zone */

/* Forward declarations */
static void notification_list_rebuild(void);

/* ── Detail overlay (full message) ────────────────────────────────── */

static void notification_detail_close(void)
{
    if (g_notif_detail && lv_obj_is_valid(g_notif_detail)) {
        lv_obj_delete_async(g_notif_detail);   /* safe from within an event cb */
    }
    g_notif_detail = NULL;
}

static void notif_detail_click_cb(lv_event_t *e)
{
    /* Tap on the dim background (not a button) closes the detail. */
    if (!g_notif_detail) return;
    if (lv_event_get_target(e) == g_notif_detail) {
        notification_detail_close();
    }
}

static void notif_detail_dismiss_cb(lv_event_t *e)
{
    notification_detail_close();
}

static void notif_detail_mark_read_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_notification_store_remove(idx);
    notification_detail_close();
    notification_list_rebuild();
}

static void notification_detail_open(int idx)
{
    const app_notification_t *n = app_notification_store_get(idx);
    if (!n || g_notif_detail) return;

    /* The real-time frosted panel is already blurred behind — the detail
     * only adds a dark scrim to focus the message + buttons. */
    g_notif_detail = lv_obj_create(g_blur_scr);
    lv_obj_remove_style_all(g_notif_detail);
    lv_obj_set_size(g_notif_detail, SCR_W, SCR_H);
    lv_obj_set_pos(g_notif_detail, 0, 0);
    lv_obj_add_flag(g_notif_detail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_notif_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_notif_detail, LV_OBJ_FLAG_OVERFLOW_VISIBLE); /* clip children */
    lv_obj_set_style_bg_color(g_notif_detail, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_notif_detail, LV_OPA_40, 0);  /* scrim over the blur */
    lv_obj_set_style_radius(g_notif_detail, 0, 0);
    lv_obj_set_style_pad_all(g_notif_detail, 0, 0);
    lv_obj_add_event_cb(g_notif_detail, notif_detail_click_cb, LV_EVENT_CLICKED, NULL);

    /* Content column — flex layout keeps the buttons clear of the message. */
    lv_obj_t *content = lv_obj_create(g_notif_detail);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, SCR_W, SCR_H);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(content, 24, 0);

    /* Large round icon — scaled inside a circular clipped container. */
    const lv_image_dsc_t *icon_src = app_notification_store_resolve_icon(n->app_id);
    if (!icon_src) icon_src = app_notification_store_default_icon();
    lv_obj_t *icon_box = lv_obj_create(content);
    lv_obj_set_size(icon_box, NOTIF_ICON_LARGE_SIZE, NOTIF_ICON_LARGE_SIZE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(icon_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(icon_box, true, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_t *icon = lv_image_create(icon_box);
    lv_image_set_src(icon, icon_src);
    lv_image_set_scale(icon, NOTIF_ICON_LARGE_SIZE * LV_SCALE_NONE / 96);
    lv_obj_center(icon);

    /* Title (app / sender name) — centered */
    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, (n->title[0]) ? n->title : n->app_id);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &noto_font_48, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(title, 12, 0);

    /* Full message — centered, wraps; capped so the buttons never overlap. */
    lv_obj_t *msg = lv_label_create(content);
    lv_label_set_text(msg, (n->message[0]) ? n->message : "");
    lv_obj_set_style_text_color(msg, lv_color_hex(0xD8DCE3), 0);
    lv_obj_set_style_text_font(msg, &noto_font_24, 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_max_height(msg, 100, 0);
    lv_obj_set_style_margin_top(msg, 12, 0);

    /* Relative time — centered */
    char time_buf[32];
    app_notification_store_format_time(n->timestamp, time_buf, sizeof(time_buf));
    lv_obj_t *time_lbl = lv_label_create(content);
    lv_label_set_text(time_lbl, time_buf);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(0x9BA0AA), 0);
    lv_obj_set_style_text_font(time_lbl, &noto_font_14, 0);
    lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(time_lbl, 6, 0);

    /* Spacer — pushes the buttons down for short messages, collapses for
     * long ones so the buttons follow the message and never overlap it. */
    lv_obj_t *spacer = lv_obj_create(content);
    lv_obj_remove_style_all(spacer);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    /* Mark as read button */
    lv_obj_t *mark_btn = lv_button_create(content);
    lv_obj_set_size(mark_btn, lv_pct(100), 72);
    lv_obj_remove_flag(mark_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(mark_btn, lv_color_hex(0x242424), 0);
    lv_obj_set_style_radius(mark_btn, 36, 0);
    lv_obj_set_style_border_width(mark_btn, 0, 0);
    lv_obj_set_style_margin_bottom(mark_btn, 10, 0);
    lv_obj_add_event_cb(mark_btn, notif_detail_mark_read_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);
    lv_obj_t *mark_lbl = lv_label_create(mark_btn);
    lv_label_set_text(mark_lbl, "Mark as Read");
    lv_obj_set_style_text_color(mark_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(mark_lbl, &noto_font_24, 0);
    lv_obj_center(mark_lbl);

    /* Dismiss button */
    lv_obj_t *dismiss_btn = lv_button_create(content);
    lv_obj_set_size(dismiss_btn, lv_pct(100), 72);
    lv_obj_remove_flag(dismiss_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(dismiss_btn, lv_color_hex(0x242424), 0);
    lv_obj_set_style_radius(dismiss_btn, 36, 0);
    lv_obj_set_style_border_width(dismiss_btn, 0, 0);
    lv_obj_add_event_cb(dismiss_btn, notif_detail_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dismiss_lbl = lv_label_create(dismiss_btn);
    lv_label_set_text(dismiss_lbl, "Dismiss");
    lv_obj_set_style_text_color(dismiss_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(dismiss_lbl, &noto_font_24, 0);
    lv_obj_center(dismiss_lbl);

    lv_obj_fade_in(g_notif_detail, 150, 0);
}

/* ── Notification card ────────────────────────────────────────────── */

static void notif_item_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    notification_detail_open(idx);
}

static void notification_item_create(lv_obj_t *parent, const app_notification_t *n, int idx)
{
    /* Card container — height auto-adjusts to message length */
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_flag(container, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x242424), 0);
    lv_obj_set_style_bg_opa(container, NOTIF_ITEM_BG_OPA, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_margin_bottom(container, NOTIF_ITEM_MARGIN, 0);
    lv_obj_set_style_pad_all(container, NOTIF_ITEM_PAD, 0);
    lv_obj_set_style_radius(container, NOTIF_ITEM_RADIUS, 0);
    lv_obj_set_style_shadow_width(container, 0, 0);
    lv_obj_set_user_data(container, (void *)(intptr_t)(idx + 1));   /* card marker */
    lv_obj_add_event_cb(container, notif_item_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);

    /* Row 1: icon | title + time */
    lv_obj_t *row1 = lv_obj_create(container);
    lv_obj_set_size(row1, lv_pct(100), NOTIF_ICON_SIZE);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row1, 0, 0);
    lv_obj_set_style_pad_all(row1, 0, 0);
    lv_obj_remove_flag(row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_SPACE_BETWEEN);
    /* row1 is clickable by default — forward its taps to the card so the
     * whole card (icon/title/time area too) opens the detail. */
    lv_obj_add_flag(row1, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* App icon — scaled inside a circular container that clips to a circle. */
    const lv_image_dsc_t *icon_src = app_notification_store_resolve_icon(n->app_id);
    if (!icon_src) icon_src = app_notification_store_default_icon();
    lv_obj_t *icon_box = lv_obj_create(row1);
    lv_obj_set_size(icon_box, NOTIF_ICON_SIZE, NOTIF_ICON_SIZE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(icon_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(icon_box, true, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_set_style_margin_right(icon_box, 16, 0);
    lv_obj_t *icon = lv_image_create(icon_box);
    lv_image_set_src(icon, icon_src);
    lv_image_set_scale(icon, NOTIF_ICON_SIZE * LV_SCALE_NONE / 96);
    lv_obj_center(icon);

    /* Title (app name) */
    lv_obj_t *title = lv_label_create(row1);
    lv_label_set_text(title, (n->title[0]) ? n->title : n->app_id);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &noto_font_24, 0);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);

    /* Relative time */
    char time_buf[32];
    app_notification_store_format_time(n->timestamp, time_buf, sizeof(time_buf));
    lv_obj_t *time_label = lv_label_create(row1);
    lv_label_set_text(time_label, time_buf);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x9BA0AA), 0);
    lv_obj_set_style_text_font(time_label, &noto_font_14, 0);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_RIGHT, 0);

    /* Message — wraps so the card grows with the text */
    lv_obj_t *msg = lv_label_create(container);
    lv_label_set_text(msg, (n->message[0]) ? n->message : "");
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_color(msg, lv_color_hex(0xAEB4BF), 0);
    lv_obj_set_style_text_font(msg, &noto_font_24, 0);
    lv_obj_set_style_margin_top(msg, 16, 0);
    lv_obj_set_style_text_line_space(msg, 2, 0);
}

/* ── Clear-all button ─────────────────────────────────────────────── */

static void notif_clear_all_click_cb(lv_event_t *e)
{
    app_notification_store_clear();
    notification_list_rebuild();
    app_events_request_vibe_click();
}

static lv_obj_t *notification_clear_all_create(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, lv_pct(100), 80);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x242424), 0);
    lv_obj_set_style_bg_opa(btn, NOTIF_ITEM_BG_OPA, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_margin_bottom(btn, NOTIF_ITEM_MARGIN, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_radius(btn, 50, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, notif_clear_all_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "Clear All");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &noto_font_24, 0);
    lv_obj_center(label);
    return btn;
}

/* ── List build / rebuild ─────────────────────────────────────────── */

/** Delete every card item child of the list (keeps the clear-all button). */
static void notification_list_clear_items(void)
{
    if (!g_notif_list) return;
    uint32_t cnt = lv_obj_get_child_count(g_notif_list);
    lv_obj_t **children = malloc(sizeof(lv_obj_t *) * (cnt + 1));
    if (!children) return;
    for (uint32_t i = 0; i < cnt; i++) {
        children[i] = lv_obj_get_child(g_notif_list, i);
    }
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = children[i];
        if (child == g_notif_clear_all) continue;
        lv_obj_delete(child);
    }
    free(children);
}

static void notification_list_rebuild(void)
{
    if (!g_notif_list || !g_blur_scr) return;

    notification_list_clear_items();

    int count = app_notification_store_count();
    if (count <= 0) {
        if (g_notif_clear_all) lv_obj_add_flag(g_notif_clear_all, LV_OBJ_FLAG_HIDDEN);
        if (g_notif_no_msg)    lv_obj_remove_flag(g_notif_no_msg, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (g_notif_clear_all) lv_obj_remove_flag(g_notif_clear_all, LV_OBJ_FLAG_HIDDEN);
    if (g_notif_no_msg)    lv_obj_add_flag(g_notif_no_msg, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < count; i++) {
        const app_notification_t *n = app_notification_store_get(i);
        if (n) notification_item_create(g_notif_list, n, i);
    }
    lv_obj_update_layout(g_notif_list);
}

/** Create the whole notification list UI on the blur screen. */
static void notification_list_create(void)
{
    if (!g_blur_scr) return;

    /* No-notification placeholder (centered on the screen). */
    g_notif_no_msg = lv_label_create(g_blur_scr);
    lv_label_set_text(g_notif_no_msg, "No Notifications");
    lv_obj_set_style_text_color(g_notif_no_msg, lv_color_hex(0x9BA0AA), 0);
    lv_obj_set_style_text_font(g_notif_no_msg, &noto_font_24, 0);
    lv_obj_center(g_notif_no_msg);
    lv_obj_add_flag(g_notif_no_msg, LV_OBJ_FLAG_HIDDEN);

    /* Scrollable list container (full screen, hidden until open). */
    g_notif_list = lv_obj_create(g_blur_scr);
    lv_obj_remove_style_all(g_notif_list);
    lv_obj_set_size(g_notif_list, SCR_W, SCR_H);
    lv_obj_set_pos(g_notif_list, 0, 0);
    lv_obj_set_scroll_dir(g_notif_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_notif_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(g_notif_list, NOTIF_LIST_PAD, 0);
    lv_obj_set_style_pad_top(g_notif_list, NOTIF_LIST_PAD_TOP, 0);
    lv_obj_set_flex_flow(g_notif_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_notif_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(g_notif_list, LV_OBJ_FLAG_HIDDEN);

    /* Clear-all button is always the first child. */
    g_notif_clear_all = notification_clear_all_create(g_notif_list);
    lv_obj_add_flag(g_notif_clear_all, LV_OBJ_FLAG_HIDDEN);

    notification_list_rebuild();
}

static void notification_list_show(void)
{
    if (g_notif_list) {
        lv_obj_remove_flag(g_notif_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_fade_in(g_notif_list, 200, 0);
    }
}

/* ── Real-time blur overlay (native LVGL backdrop blur) ───────────── */

/* ── Snapshot + blur + create blur screen ────────────────────────── */

static void create_blur_screen(void)
{
    /* Attach the notification-center overlay to the LIVE active screen.
     * The frosted panel uses LVGL's native backdrop blur, so whatever is on
     * screen behind it keeps rendering and is blurred in real time — no
     * snapshot, no manual box blur. */
    lv_obj_t *src_scr = lv_screen_active();
    if (!src_scr) return;

    g_prev_scr = src_scr;

    /* Overlay container: full-screen, transparent, non-clickable so touches
     * outside the panel/list still reach the watch UI behind it. */
    g_blur_scr = lv_obj_create(src_scr);
    lv_obj_remove_style_all(g_blur_scr);
    lv_obj_set_size(g_blur_scr, SCR_W, SCR_H);
    lv_obj_set_pos(g_blur_scr, 0, 0);
    lv_obj_remove_flag(g_blur_scr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(g_blur_scr, LV_OPA_TRANSP, 0);

    /* Frosted panel: its height tracks the pull-down distance (g_cur_y), so
     * only the pulled-down region is blurred.  Transparent fill = pure
     * frosted glass (no dimming), matching the previous behaviour. */
    g_blur_panel = lv_obj_create(g_blur_scr);
    lv_obj_remove_style_all(g_blur_panel);
    lv_obj_set_size(g_blur_panel, SCR_W, g_cur_y);
    lv_obj_set_pos(g_blur_panel, 0, 0);
    lv_obj_remove_flag(g_blur_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(g_blur_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(g_blur_panel, 0, 0);
    lv_obj_set_style_border_width(g_blur_panel, 0, 0);
    lv_obj_set_style_blur_radius(g_blur_panel, BLUR_RADIUS, 0);
    lv_obj_set_style_blur_backdrop(g_blur_panel, true, 0);
    lv_obj_set_style_blur_quality(g_blur_panel, LV_BLUR_QUALITY_SPEED, 0);

    /* Notification list UI on top of the blurred panel. */
    notification_list_create();

    ESP_LOGI(TAG, "Real-time blur overlay ready (panel h=%d)", (int)g_cur_y);
}

static void destroy_blur_screen(void)
{
    /* Delete the overlay and all its children (panel, list, detail). */
    if (g_blur_scr && lv_obj_is_valid(g_blur_scr)) {
        lv_obj_delete(g_blur_scr);
    }
    g_blur_scr      = NULL;
    g_blur_panel    = NULL;
    g_notif_list      = NULL;
    g_notif_clear_all = NULL;
    g_notif_no_msg    = NULL;
    g_notif_detail    = NULL;
    g_bottom_swipe_armed = false;
    g_started_at_edge = false;

    /* The watch screen was never left — no screen reload needed. */
    g_prev_scr = NULL;

    ESP_LOGI(TAG, "Blur overlay destroyed");
}

/* ── Input device callback ────────────────────────────────────────── */

static void indev_press_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    bool valid_pt = (pt.x >= 0 && pt.y >= 0 && pt.x < SCR_W && pt.y < SCR_H);
    bool pressed = false;
    if ((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) && valid_pt) pressed = true;
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) pressed = false;

    const char *event_name = "UNKNOWN";
    switch (code) {
        case LV_EVENT_PRESSED: event_name = "PRESSED"; break;
        case LV_EVENT_PRESSING: event_name = "PRESSING"; break;
        case LV_EVENT_RELEASED: event_name = "RELEASED"; break;
        case LV_EVENT_PRESS_LOST: event_name = "PRESS_LOST"; break;
        default: break;
    }

    /* Rate-limited debug log */
    static lv_point_t last_pt = {-1, -1};
    static bool last_pressed = false;
    if (pressed != last_pressed || pt.x != last_pt.x || pt.y != last_pt.y) {
        ESP_LOGI(TAG, "Touch: evt=%s press=%d x=%" PRIi32 " y=%" PRIi32
                 " edge=%s%s", event_name, pressed, pt.x, pt.y,
                 (!valid_pt) ? "INVALID" :
                 ((pt.y <= SWIPE_EDGE) ? "YES" : "no"),
                 (!valid_pt) ? " (skip)" : "");
        last_pt = pt;
        last_pressed = pressed;
    }

    switch (gs) {

    /* ── IDLE: waiting for top-edge touch ─────────────────────── */
    case GS_IDLE:
        /* Don't open over the watch-face picker gallery */
        if ((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) && valid_pt &&
            pt.y <= SWIPE_EDGE && g_current_screen != APP_SCREEN_WATCH_FACE_PICKER) {
            ESP_LOGI(TAG, ">>> Swipe start");
            g_cur_y = pt.y;
            create_blur_screen();
            if (g_blur_scr) {
                g_started_at_edge = true;
                gs = GS_TRACKING;
                g_start_y = pt.y;
                g_start_t0 = esp_timer_get_time();
                tint_set_opa(LV_OPA_0);
            }
        }
        break;

    /* ── TRACKING: finger dragging, update dim ─────────────────── */
    case GS_TRACKING:
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            /* Finger released — decide expand vs collapse */
            bool open = false;
            if (g_cur_y >= OPEN_THRESHOLD) {
                open = true;
            } else {
                int64_t dt_us = esp_timer_get_time() - g_start_t0;
                int32_t dy = g_cur_y - g_start_y;
                if (dt_us > 0 && dy > 0) {
                    int32_t vel = (int32_t)((dy * 1000000LL) / dt_us);
                    if (vel >= MIN_SWIPE_VELOCITY && g_cur_y >= SWIPE_EDGE) {
                        open = true;
                        ESP_LOGI(TAG, "OPEN by fling: dy=%d dt=%lld vel=%d", dy, dt_us, vel);
                    }
                }
            }

            if (open) {
                /* animate panel height to full screen */
                gs = GS_EXPANDING;
                g_anim_t0 = esp_timer_get_time();
                g_anim_h0 = g_cur_y;
                g_anim_h_target = SCR_H;
                g_anim_ms = EXPAND_MS;
                ESP_LOGI(TAG, "EXPANDING from %d to %d", g_anim_h0, g_anim_h_target);
            } else {
                /* animate collapse to 0 */
                gs = GS_COLLAPSING;
                g_anim_t0 = esp_timer_get_time();
                g_anim_h0 = g_cur_y;
                g_anim_h_target = 0;
                g_anim_ms = EXPAND_MS;
                g_anim_opa0 = y_to_opa(g_cur_y);
                ESP_LOGI(TAG, "COLLAPSING (opa was %d)", g_anim_opa0);
            }
            break;
        }
        /* Still dragging — update y and dim opacity */
        if (code == LV_EVENT_PRESSING && g_started_at_edge) {
            g_cur_y = (pt.y < 0) ? 0 : pt.y;
            if (g_blur_panel) {
                lv_obj_set_height(g_blur_panel, g_cur_y);
            }
            tint_set_opa(y_to_opa(g_cur_y));
        }
        break;

    /* ── OPEN: blur panel fully open, waiting for dismiss gesture ─── */
    case GS_OPEN:
        if (code == LV_EVENT_PRESSED) {
            g_press_x0 = pt.x;
            g_press_y0 = pt.y;
            /* Only a press that starts in the bottom zone can exit via swipe. */
            g_bottom_swipe_armed = (pt.y >= SCR_H - BOTTOM_SWIPE_EDGE);
        } else if (code == LV_EVENT_RELEASED) {
            /* The center can only be exited by GPIO0 (handled in the button
             * path) or by swiping up from the bottom edge.  Taps are left to
             * the widgets (open detail / clear-all); empty-area taps do
             * nothing; scrolling the list is untouched. */
            if (!pressed && g_was_pressed && g_bottom_swipe_armed) {
                int dy = pt.y - g_press_y0;
                if (dy <= -SWIPE_UP_MIN_DIST) {
                    if (g_notif_detail) {
                        /* Nested: swipe closes the detail first. */
                        notification_detail_close();
                    } else {
                        ESP_LOGI(TAG, "CLOSE (swipe up from bottom)");
                        gs = GS_COLLAPSING;
                        g_anim_t0 = esp_timer_get_time();
                        g_anim_h0 = SCR_H;
                        g_anim_h_target = 0;
                        g_anim_ms = DISMISS_MS;
                        g_anim_opa0 = MAX_DIM_OPA;
                    }
                }
            }
        }
        break;

    /* ── EXPANDING / COLLAPSING: animations handled in process() */
    case GS_EXPANDING:
    case GS_COLLAPSING:
        break;
    }

    g_was_pressed = pressed;
}

/* ── Public API ──────────────────────────────────────────────────── */

void app_notification_center_init(void)
{
    /* Register on the input device so we catch ALL touches regardless
     * of which LVGL screen is active. */
    lv_indev_t *indev = hw_touch_get_indev();
    if (indev) {
        lv_indev_add_event_cb(indev, indev_press_cb,
                               LV_EVENT_ALL, NULL);
        g_indev_cb_added = true;
    } else {
        g_indev_cb_added = false;
    }

    /* Also enable bubbling on the active screen as fallback */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_add_event_cb(scr, indev_press_cb, LV_EVENT_PRESSING, NULL);
    enable_bubble_recursive(scr);

    ESP_LOGI(TAG, "ready (%dx%d blur_r=%d)", SCR_W, SCR_H, BLUR_RADIUS);
}

void app_notification_center_process(void)
{
    int64_t now = esp_timer_get_time();

    /* If indev callback wasn't available at init, try to add it now.
     * This fixes the case where touch driver initializes after this module,
     * causing the notification pull-down to only work on early screens. */
    if (!g_indev_cb_added) {
        lv_indev_t *id = hw_touch_get_indev();
        if (id) {
            lv_indev_add_event_cb(id, indev_press_cb, LV_EVENT_ALL, NULL);
            g_indev_cb_added = true;
            ESP_LOGI(TAG, "input device callback registered (late)");
        }
    }

    /* Ensure the active screen has a pressing callback and bubbling enabled.
     * Some screens are loaded after init; re-attach handler on screen change. */
    lv_obj_t *active = lv_screen_active();
    if (active && active != g_last_active_scr) {
        lv_obj_add_event_cb(active, indev_press_cb, LV_EVENT_PRESSING, NULL);
        enable_bubble_recursive(active);
        g_last_active_scr = active;
        ESP_LOGI(TAG, "attached screen pressing cb to active screen");
    }

    if (gs == GS_EXPANDING) {
        int64_t el = now - g_anim_t0;
        if (el >= (int64_t)g_anim_ms * 1000) {
            /* finished expanding */
            if (g_blur_panel) lv_obj_set_height(g_blur_panel, SCR_H);
            tint_set_opa(LV_OPA_0);
            notification_list_show();
            gs = GS_OPEN;
            ESP_LOGI(TAG, "EXPAND done");
        } else {
            float p = (float)el / (g_anim_ms * 1000.0f);
            int32_t h = g_anim_h0 + (int32_t)((g_anim_h_target - g_anim_h0) * p);
            if (g_blur_panel) lv_obj_set_height(g_blur_panel, h);
            /* keep no dimming */
            tint_set_opa(LV_OPA_0);
        }
    }

    if (gs == GS_COLLAPSING) {
        int64_t el = now - g_anim_t0;
        if (el >= (int64_t)g_anim_ms * 1000) {
            /* finished collapsing */
            destroy_blur_screen();
            gs = GS_IDLE;
            ESP_LOGI(TAG, "COLLAPSE done");
        } else {
            float p = (float)el / (g_anim_ms * 1000.0f);
            if (p > 1.0f) p = 1.0f;
            int32_t h = g_anim_h0 + (int32_t)((g_anim_h_target - g_anim_h0) * p);
            if (g_blur_panel) lv_obj_set_height(g_blur_panel, h);
            /* no dimming while collapsing */
            tint_set_opa(LV_OPA_0);
        }
    }
}

bool app_notification_center_is_open(void)
{
    return (gs == GS_OPEN || gs == GS_TRACKING || gs == GS_EXPANDING);
}

void app_notification_center_refresh(void)
{
    /* If the list UI already exists (center open / expanding), rebuild it.
     * Otherwise the next open will build it fresh. */
    if (g_notif_list && lv_obj_is_valid(g_notif_list)) {
        notification_list_rebuild();
    }
}

void app_notification_center_dismiss(void)
{
    if (gs == GS_OPEN || gs == GS_TRACKING || gs == GS_EXPANDING) {
        ESP_LOGI(TAG, "Force dismiss via back button");
        bool was_open = (gs == GS_OPEN);
        gs = GS_COLLAPSING;
        g_anim_t0 = esp_timer_get_time();
        g_anim_h0 = was_open ? SCR_H : g_cur_y;
        g_anim_h_target = 0;
        g_anim_ms = DISMISS_MS;
        g_anim_opa0 = was_open ? MAX_DIM_OPA : y_to_opa(g_cur_y);
    }
}
