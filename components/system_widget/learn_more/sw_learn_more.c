/**
 * @file sw_learn_more.c
 * @brief Blue "Learn more..." link label (see sw_learn_more.h).
 */

#include "sw_learn_more.h"

lv_obj_t *sw_learn_more_add(lv_obj_t *list, lv_event_cb_t cb)
{
    lv_obj_t *l = lv_label_create(list);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_font(l, SW_FONT_S, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(l, "Learn more...");
    lv_obj_set_style_text_color(l, SW_THEME_PRIMARY, 0);
    lv_obj_set_style_text_color(l,
        lv_color_lighten(SW_THEME_PRIMARY, 40), LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(l, EOS_WX(16), 0);
    lv_obj_set_style_pad_bottom(l, EOS_WY(8), 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, NULL);
    return l;
}
