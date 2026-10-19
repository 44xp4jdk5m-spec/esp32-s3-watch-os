/**
 * @file sw_icon_btn.c
 * @brief Round-icon + label row (see sw_icon_btn.h).
 */

#include "sw_icon_btn.h"
#include "sw_row.h"

lv_obj_t *sw_icon_btn_add(lv_obj_t *list, lv_color_t bg,
                          const void *icon, const char *txt,
                          lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = sw_row_create(list);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_USER_1);
    lv_obj_set_style_pad_left(btn, EOS_WX(14), 0);
    sw_row_round_icon(btn, bg, icon);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, SW_FONT_M, 0);
    lv_obj_set_style_margin_left(label, EOS_WX(14), 0);
    lv_label_set_text(label, txt);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_margin_right(label, EOS_WX(14), 0);
    lv_obj_set_style_text_color(label, SW_COLOR_WHITE, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    return btn;
}
