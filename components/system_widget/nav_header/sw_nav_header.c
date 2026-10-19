/**
 * @file sw_nav_header.c
 * @brief Floating navigation header (see sw_nav_header.h).
 *
 * Ported from the settings header: gradient fade mask, circular back
 * button with press-scale, clock slot and the cross-fading title.
 */

#include "sw_nav_header.h"
#include "sw_icons.h"
#include "noto_font_24_bold.h"
#include <string.h>
#include <stdio.h>

struct sw_header {
    lv_obj_t *obj;        /* top-layer container */
    lv_obj_t *title;      /* cross-fading title label */
    lv_obj_t *clock;      /* clock label slot */
};

/* Buffer that outlives the fade-out → text-swap → fade-in sequence. */
static char s_title_buf[64];

/* ── Title cross-fade ──────────────────────────────────────────────── */

static void title_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void title_fade_done_cb(lv_anim_t *a)
{
    lv_obj_t *lbl = (lv_obj_t *)a->var;
    const char *title = (const char *)a->user_data;
    if (title) lv_label_set_text(lbl, title);
    /* Fade back in. */
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, lbl);
    lv_anim_set_exec_cb(&b, title_opa_cb);
    lv_anim_set_values(&b, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&b, 120);
    lv_anim_start(&b);
}

static void sw_header_fade_title(lv_obj_t *title, const char *text)
{
    if (strcmp(lv_label_get_text(title), text) == 0) return;

    /* Cancel any in-flight fade, then fade out → swap → fade in. */
    lv_anim_del(title, title_opa_cb);
    snprintf(s_title_buf, sizeof(s_title_buf), "%s", text);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, title);
    lv_anim_set_exec_cb(&a, title_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 120);
    lv_anim_set_completed_cb(&a, title_fade_done_cb);
    lv_anim_set_user_data(&a, s_title_buf);
    lv_anim_start(&a);
}

/* ── Header construction ───────────────────────────────────────────── */

sw_header_t *sw_header_create(const char *title, lv_event_cb_t back_cb,
                              void *back_ud)
{
    sw_header_t *h = lv_malloc(sizeof(sw_header_t));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));

    h->obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(h->obj, EOS_DISPLAY_W, SW_HDR_H);
    lv_obj_align(h->obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(h->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h->obj, 0, 0);
    lv_obj_set_style_pad_all(h->obj, 0, 0);
    lv_obj_set_style_radius(h->obj, 0, 0);
    lv_obj_remove_flag(h->obj, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Header fade mask ────────────────────────────────────────── */
    lv_obj_t *mask = lv_obj_create(h->obj);
    lv_obj_set_size(mask, EOS_DISPLAY_W, SW_HDR_MASK_H);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(mask, SW_COLOR_BLACK, 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(mask, SW_COLOR_WHITE, 0);
    lv_obj_set_style_bg_grad_dir(mask, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(mask, 175, 0);
    lv_obj_set_style_bg_grad_stop(mask, 255, 0);
    lv_obj_set_style_blend_mode(mask, LV_BLEND_MODE_MULTIPLY, 0);
    lv_obj_set_style_opa(mask, LV_OPA_COVER, 0);

    /* ── Back button ─────────────────────────────────────────────── */
    lv_obj_t *bb = lv_button_create(h->obj);
    lv_obj_set_size(bb, SW_BACK_BTN_W, SW_BACK_BTN_H);
    lv_obj_set_style_radius(bb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bb, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_border_width(bb, 0, 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_align(bb, LV_ALIGN_LEFT_MID, SW_BACK_BTN_MARGIN_L + EOS_WX(6),
                 EOS_WY(4));
    lv_obj_add_event_cb(bb, back_cb, LV_EVENT_CLICKED, back_ud);

    lv_obj_t *bl = lv_label_create(bb);
    lv_label_set_text(bl, SW_ICON_ARROW_BACK);
    lv_obj_set_style_text_font(bl, SW_ICON_F, 0);
    lv_obj_set_style_text_color(bl, SW_COLOR_WHITE, 0);
    lv_obj_align(bl, LV_ALIGN_CENTER, -EOS_WX(2), EOS_WY(2));

    /* Back btn press scale = 350 (Apple Watch). */
    lv_obj_set_style_transform_pivot_x(bb, SW_BACK_BTN_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(bb, SW_BACK_BTN_H / 2, 0);
    static lv_style_t st_bbp; static bool st_bbi;
    if (!st_bbi) {
        lv_style_init(&st_bbp);
        lv_style_set_transform_scale(&st_bbp, 350);
        lv_style_set_bg_color(&st_bbp,
                              lv_color_lighten(SW_THEME_BUTTON, 64));
        st_bbi = true;
    }
    lv_obj_add_style(bb, &st_bbp, LV_STATE_PRESSED);

    /* ── Clock label slot ────────────────────────────────────────── */
    h->clock = lv_label_create(h->obj);
    lv_obj_set_style_text_font(h->clock, &noto_font_24_bold, 0);
    lv_obj_set_style_text_color(h->clock, SW_COLOR_WHITE, 0);
    lv_obj_align(h->clock, LV_ALIGN_RIGHT_MID, -SW_HDR_MARGIN_R, -EOS_WY(14));

    /* ── Title label — right-aligned, cross-fades ─────────────────── */
    h->title = lv_label_create(h->obj);
    lv_obj_set_width(h->title, SW_HDR_TITLE_W);
    lv_obj_set_style_text_font(h->title, SW_FONT_M, 0);
    lv_label_set_long_mode(h->title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(h->title, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(h->title, SW_THEME_PRIMARY, 0);
    lv_obj_set_style_opa(h->title, LV_OPA_COVER, 0);
    lv_obj_align(h->title, LV_ALIGN_RIGHT_MID, SW_HDR_TITLE_X_OFF,
                 SW_HDR_TITLE_Y_OFF);
    lv_label_set_text(h->title, title);
    return h;
}

void sw_header_set_title(sw_header_t *h, const char *title)
{
    if (!h || !h->title || !lv_obj_is_valid(h->title)) return;
    sw_header_fade_title(h->title, title);
}

void sw_header_set_visible(sw_header_t *h, bool visible)
{
    if (!h || !h->obj || !lv_obj_is_valid(h->obj)) return;
    if (visible)
        lv_obj_remove_flag(h->obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(h->obj, LV_OBJ_FLAG_HIDDEN);
}

bool sw_header_is_visible(const sw_header_t *h)
{
    return h && h->obj && lv_obj_is_valid(h->obj) &&
           !lv_obj_has_flag(h->obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *sw_header_clock_label(sw_header_t *h)
{
    return h ? h->clock : NULL;
}

lv_obj_t *sw_header_title_label(sw_header_t *h)
{
    return h ? h->title : NULL;
}

void sw_header_destroy(sw_header_t *h)
{
    if (!h) return;
    if (h->obj && lv_obj_is_valid(h->obj)) {
        if (h->title && lv_obj_is_valid(h->title)) {
            lv_anim_del(h->title, title_opa_cb);
        }
        lv_obj_del(h->obj);
    }
    lv_free(h);
}
