/**
 * @file sw_slider.c
 * @brief Stepper + font-size sliders (see sw_slider.h).
 */

#include "sw_slider.h"
#include <stdlib.h>

/* ====================================================================
 * Generic − / ＋ stepper slider
 * ==================================================================== */

static void sw_slider_delete_cb(lv_event_t *e);

static void sw_slider_btn_cb(lv_event_t *e)
{
    sw_slider_t *ls = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_target(e);
    if (!ls || !ls->slider) return;
    int32_t mn = lv_slider_get_min_value(ls->slider);
    int32_t mx = lv_slider_get_max_value(ls->slider);
    int32_t v  = lv_slider_get_value(ls->slider);
    if (t == ls->minus_btn || t == ls->minus_label ||
        lv_obj_get_parent(t) == ls->minus_btn) {
        if (v > mn) { v = (v - 5 < mn) ? mn : v - 5; lv_slider_set_value(ls->slider, v, LV_ANIM_ON); }
    } else {
        if (v < mx) { v = (v + 5 > mx) ? mx : v + 5; lv_slider_set_value(ls->slider, v, LV_ANIM_ON); }
    }
    lv_obj_send_event(ls->slider, LV_EVENT_VALUE_CHANGED, NULL);
}

static lv_obj_t *sw_slider_stepper(lv_obj_t *inner, const void *sym,
                                   lv_align_t align, sw_slider_t *ls,
                                   lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_obj_create(inner);
    lv_obj_set_size(btn, lv_pct(25), SW_ROW_H);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, SW_THEME_BUTTON, 0);
    lv_obj_set_style_radius(btn, SW_ITEM_RADIUS, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_align_to(btn, inner, align, 0, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, (const char *)sym);
    lv_obj_set_style_text_font(label, SW_ICON_F, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, sw_slider_btn_cb, LV_EVENT_CLICKED, ls);
    *label_out = label;
    return btn;
}

sw_slider_t *sw_slider_add(lv_obj_t *list, const char *title,
                           const void *minus_sym, const void *plus_sym)
{
    sw_slider_t *ls = calloc(1, sizeof(sw_slider_t));
    if (!ls) return NULL;

    /* Rounded panel container for the stepper bar. */
    lv_obj_t *inner = lv_obj_create(list);
    lv_obj_remove_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(inner, LV_DIR_NONE);
    lv_obj_set_style_bg_color(inner, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(inner, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(inner, 0, 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_set_size(inner, lv_pct(100), SW_ROW_H);

    /* Title label — left-aligned to match switch rows. */
    lv_obj_t *tl = lv_label_create(list);
    lv_obj_set_width(tl, lv_pct(100));
    lv_obj_set_style_text_font(tl, SW_FONT_M, 0);
    lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_color(tl, SW_COLOR_GREY, 0);
    lv_obj_set_style_pad_left(tl, EOS_WX(16), 0);
    lv_obj_set_style_pad_top(tl, EOS_WY(8), 0);
    lv_obj_add_flag(tl, LV_OBJ_FLAG_USER_1);

    /* Minus btn */
    ls->minus_btn = sw_slider_stepper(inner, minus_sym, LV_ALIGN_LEFT_MID,
                                      ls, &ls->minus_label);

    /* Plus btn */
    ls->plus_btn = sw_slider_stepper(inner, plus_sym, LV_ALIGN_RIGHT_MID,
                                     ls, &ls->plus_label);

    /* Slider centre */
    lv_obj_t *mid = lv_obj_create(inner);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(mid, LV_DIR_NONE);
    lv_obj_set_style_bg_color(mid, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_opa(mid, LV_OPA_COVER, 0);
    lv_obj_set_size(mid, lv_pct(50), SW_ROW_H + 4);
    lv_obj_set_style_border_width(mid, 2, 0);
    lv_obj_set_style_border_color(mid, SW_COLOR_BLACK, 0);
    lv_obj_set_style_radius(mid, 0, 0);
    lv_obj_center(mid);

    ls->slider = lv_slider_create(mid);
    lv_obj_remove_flag(ls->slider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_margin_left(ls->slider, EOS_WX(18), 0);
    lv_obj_set_size(ls->slider, lv_pct(100), 5);
    lv_obj_set_style_margin_top(ls->slider, EOS_WY(12), 0);
    lv_obj_center(ls->slider);
    lv_obj_set_style_bg_opa(ls->slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_opa(ls->slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_color(ls->slider,
                              lv_color_darken(SW_COLOR_GREEN, 180),
                              LV_PART_MAIN);

    lv_obj_add_flag(ls->minus_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(ls->plus_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* Free the struct when the slider's container is deleted. */
    lv_obj_add_event_cb(inner, sw_slider_delete_cb, LV_EVENT_DELETE, ls);
    return ls;
}

void sw_slider_free(sw_slider_t *s)
{
    free(s);
}

/* Auto-free the slider struct when the list (and its slider children) are
 * deleted — prevents a leak on every in-place list refresh. */
static void sw_slider_delete_cb(lv_event_t *e)
{
    sw_slider_t *ls = lv_event_get_user_data(e);
    if (ls) free(ls);
}

/* ====================================================================
 * Font-size slider (3 discrete steps, "Aa" steppers + tick marks)
 * ==================================================================== */

typedef struct {
    lv_obj_t *slider;
    lv_obj_t *minus_btn, *plus_btn;
} sw_font_slider_t;

static void sw_font_slider_btn_cb(lv_event_t *e)
{
    sw_font_slider_t *fs = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_target(e);
    if (!fs || !fs->slider) return;
    int32_t v = lv_slider_get_value(fs->slider);
    if (t == fs->minus_btn || lv_obj_get_parent(t) == fs->minus_btn) {
        if (v > 0) v--;
    } else {
        if (v < 2) v++;
    }
    lv_slider_set_value(fs->slider, v, LV_ANIM_ON);
    lv_obj_send_event(fs->slider, LV_EVENT_VALUE_CHANGED, NULL);
}

static void sw_font_slider_delete_cb(lv_event_t *e)
{
    sw_font_slider_t *fs = lv_event_get_user_data(e);
    if (fs) free(fs);
}

/* Overlapping "Aa" pair — "A" top-left, "a" bottom-right, overlapping in
 * the middle (the classic font-size glyph).  @p f scales the pair. */
static void sw_font_scale_pair(lv_obj_t *btn, const lv_font_t *f,
                               lv_coord_t ox, lv_coord_t oy)
{
    lv_obj_t *A = lv_label_create(btn);
    lv_label_set_text(A, "A");
    lv_obj_set_style_text_font(A, f, 0);
    lv_obj_set_style_text_color(A, SW_COLOR_WHITE, 0);
    lv_obj_align(A, LV_ALIGN_TOP_LEFT, ox, oy);
    lv_obj_add_flag(A, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *a = lv_label_create(btn);
    lv_label_set_text(a, "a");
    lv_obj_set_style_text_font(a, f, 0);
    lv_obj_set_style_text_color(a, SW_COLOR_WHITE, 0);
    lv_obj_align(a, LV_ALIGN_BOTTOM_RIGHT, -ox, -oy);
    lv_obj_add_flag(a, LV_OBJ_FLAG_EVENT_BUBBLE);
}

lv_obj_t *sw_font_slider_add(lv_obj_t *list, const char *title,
                             lv_event_cb_t cb)
{
    sw_font_slider_t *fs = calloc(1, sizeof(sw_font_slider_t));
    if (!fs) return NULL;

    /* Rounded panel container for the stepper bar. */
    lv_obj_t *inner = lv_obj_create(list);
    lv_obj_remove_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(inner, LV_DIR_NONE);
    lv_obj_set_style_bg_color(inner, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(inner, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(inner, 0, 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_set_size(inner, lv_pct(100), SW_ROW_H);

    /* Title label — left-aligned to match switch rows. */
    lv_obj_t *tl = lv_label_create(list);
    lv_obj_set_width(tl, lv_pct(100));
    lv_obj_set_style_text_font(tl, SW_FONT_M, 0);
    lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_color(tl, SW_COLOR_GREY, 0);
    lv_obj_set_style_pad_left(tl, EOS_WX(16), 0);
    lv_obj_set_style_pad_top(tl, EOS_WY(8), 0);
    lv_obj_add_flag(tl, LV_OBJ_FLAG_USER_1);

    /* ── Left stepper: small "Aa" ── */
    lv_obj_t *minus = lv_obj_create(inner);
    lv_obj_set_size(minus, lv_pct(25), SW_ROW_H);
    lv_obj_set_style_pad_all(minus, 0, 0);
    lv_obj_set_style_bg_opa(minus, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(minus, SW_THEME_BUTTON, 0);
    lv_obj_set_style_radius(minus, SW_ITEM_RADIUS, 0);
    lv_obj_set_style_border_width(minus, 0, 0);
    lv_obj_align_to(minus, inner, LV_ALIGN_LEFT_MID, 0, 0);
    sw_font_scale_pair(minus, SW_FONT_M, EOS_WX(6), EOS_WY(32));
    lv_obj_add_event_cb(minus, sw_font_slider_btn_cb, LV_EVENT_CLICKED, fs);
    fs->minus_btn = minus;

    /* ── Right stepper: big "Aa" ── */
    lv_obj_t *plus = lv_obj_create(inner);
    lv_obj_set_size(plus, lv_pct(25), SW_ROW_H);
    lv_obj_set_style_pad_all(plus, 0, 0);
    lv_obj_set_style_bg_opa(plus, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(plus, SW_THEME_BUTTON, 0);
    lv_obj_set_style_radius(plus, SW_ITEM_RADIUS, 0);
    lv_obj_set_style_border_width(plus, 0, 0);
    lv_obj_align_to(plus, inner, LV_ALIGN_RIGHT_MID, 0, 0);
    sw_font_scale_pair(plus, SW_FONT_L, EOS_WX(4), EOS_WY(22));
    lv_obj_add_event_cb(plus, sw_font_slider_btn_cb, LV_EVENT_CLICKED, fs);
    fs->plus_btn = plus;

    /* ── Slider centre with 3 tick marks ── */
    lv_obj_t *mid = lv_obj_create(inner);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(mid, LV_DIR_NONE);
    lv_obj_set_style_bg_color(mid, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_opa(mid, LV_OPA_COVER, 0);
    lv_obj_set_size(mid, lv_pct(50), SW_ROW_H + 4);
    lv_obj_set_style_border_width(mid, 2, 0);
    lv_obj_set_style_border_color(mid, SW_COLOR_BLACK, 0);
    lv_obj_set_style_radius(mid, 0, 0);
    lv_obj_center(mid);

    /* Three round tick marks (the three values), evenly spaced. */
    lv_obj_t *marks = lv_obj_create(mid);
    lv_obj_remove_flag(marks, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(marks, lv_pct(100), EOS_WY(12));
    lv_obj_set_style_bg_opa(marks, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(marks, 0, 0);
    lv_obj_set_style_pad_all(marks, 0, 0);
    lv_obj_center(marks);
    lv_obj_set_flex_flow(marks, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(marks, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *m = lv_obj_create(marks);
        lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(m, EOS_WX(12), EOS_WY(12));
        lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(m, SW_COLOR_GREY, 0);
        lv_obj_set_style_bg_opa(m, LV_OPA_70, 0);
        lv_obj_set_style_border_width(m, 0, 0);
    }

    lv_obj_t *sl = lv_slider_create(mid);
    lv_obj_remove_flag(sl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_margin_left(sl, EOS_WX(18), 0);
    lv_obj_set_size(sl, lv_pct(100), 5);
    lv_obj_set_style_margin_top(sl, EOS_WY(12), 0);
    lv_obj_center(sl);
    lv_obj_set_style_bg_opa(sl, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_opa(sl, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sl, SW_COLOR_SWITCH_TRACK, LV_PART_MAIN);
    lv_slider_set_range(sl, 0, 2);

    if (cb) lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
    fs->slider = sl;
    /* Free the struct when the slider's container is deleted. */
    lv_obj_add_event_cb(inner, sw_font_slider_delete_cb, LV_EVENT_DELETE, fs);
    return sl;
}
