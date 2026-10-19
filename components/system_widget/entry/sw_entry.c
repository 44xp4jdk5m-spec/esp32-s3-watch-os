/**
 * @file sw_entry.c
 * @brief Navigation row (see sw_entry.h).
 */

#include "sw_entry.h"
#include "sw_row.h"
#include "sw_icons.h"

lv_obj_t *sw_entry_add(lv_obj_t *list, const char *txt,
                       lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = sw_row_create(list);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_USER_1);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, SW_FONT_M, 0);
    lv_label_set_text(label, txt);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_color(label, SW_COLOR_WHITE, 0);
    lv_obj_set_style_pad_left(btn, EOS_WX(16), 0);
    label = lv_label_create(btn);
    lv_label_set_text(label, SW_ICON_ARROW_FWD);
    lv_obj_set_style_text_font(label, SW_ICON_F, 0);
    lv_obj_set_style_text_color(label, SW_COLOR_GREY, 0);
    lv_obj_set_style_pad_right(btn, EOS_WX(16), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    return btn;
}
