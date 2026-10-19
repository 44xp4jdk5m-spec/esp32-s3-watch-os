/**
 * @file sw_transition.c
 * @brief Page transitions + scroll-scale effect (see sw_transition.h).
 *
 * Ported from the settings UI: Apple-Watch slide transitions and the
 * scale-only scroll effect (with the CPU-saving "only touch the style when
 * it changes" guard).
 */

#include "sw_transition.h"

/* ====================================================================
 * Screen create / load
 * ==================================================================== */

lv_obj_t *sw_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, EOS_DISPLAY_W, EOS_DISPLAY_H);
    lv_obj_set_style_bg_color(scr, SW_COLOR_BLACK, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

void sw_screen_load(lv_obj_t *scr, bool back)
{
    lv_screen_load_anim_t anim = back
        ? LV_SCR_LOAD_ANIM_OUT_RIGHT
        : LV_SCR_LOAD_ANIM_OVER_LEFT;
    lv_screen_load_anim(scr, anim, 280, 0, true);
}

void sw_screen_load_instant(lv_obj_t *scr)
{
    lv_screen_load(scr);
}

/* ====================================================================
 * Scroll scale effect — scale only, no opacity dim.  Iterates the list's
 * direct children; items near the viewport bottom scale 70% → 100%.
 * ==================================================================== */

static void scroll_effect_cb(lv_timer_t *t)
{
    lv_obj_t *list = lv_timer_get_user_data(t);
    if (!list || !lv_obj_is_valid(list)) return;

    int32_t list_h = lv_obj_get_content_height(list);
    if (list_h < 40) return;

    uint32_t nc = lv_obj_get_child_count(list);
    int32_t fade_zone = list_h / 3;
    if (fade_zone < 40) fade_zone = 40;

    lv_area_t va;
    lv_obj_get_coords(list, &va);
    int32_t vp_bottom = va.y2;

    for (uint32_t i = 0; i < nc; i++) {
        lv_obj_t *c = lv_obj_get_child(list, i);
        if (!c || !lv_obj_is_valid(c)) continue;

        lv_area_t ca;
        lv_obj_get_coords(c, &ca);
        int32_t ch = ca.y2 - ca.y1;
        if (ch < 20) continue;                          /* tiny internals   */
        if (ch > SW_ROW_H + 50) continue;               /* tall pickers     */

        int32_t mid_y = (ca.y1 + ca.y2) / 2;
        int32_t dist  = vp_bottom - mid_y;
        if (dist < 0) dist = 0;
        if (dist > fade_zone) dist = fade_zone;

        int32_t tval    = dist * 256 / fade_zone;
        int32_t scale   = 179 + (256 - 179) * tval / 256;   /* 70% → 100% */

        /* Only touch the style when it actually changes — re-applying an
         * unchanged transform every 50 ms would invalidate the whole list
         * and pin the CPU. */
        if (lv_obj_get_style_transform_scale_x(c, 0) != scale) {
            lv_obj_set_style_transform_scale(c, scale, 0);
            lv_obj_set_style_transform_pivot_y(c, lv_pct(50), 0);
        }
        if (lv_obj_get_style_anim_duration(c, 0) != 100) {
            lv_obj_set_style_anim_duration(c, 100, 0);
        }
    }
}

lv_timer_t *sw_scroll_effect_start(lv_obj_t *list)
{
    lv_timer_t *t = lv_timer_create(scroll_effect_cb, 50, list);
    lv_timer_set_repeat_count(t, -1);
    return t;
}

void sw_scroll_effect_stop(lv_timer_t *t)
{
    if (t) lv_timer_del(t);
}
