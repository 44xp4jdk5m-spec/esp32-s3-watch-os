/**
 * @file sw_comment.c
 * @brief Secondary description text row (see sw_comment.h).
 */

#include "sw_comment.h"

lv_obj_t *sw_comment_add(lv_obj_t *list, const char *txt)
{
    lv_obj_t *l = lv_label_create(list);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_font(l, SW_FONT_S, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, SW_COLOR_TEXT_GREY, 0);
    lv_obj_set_style_pad_left(l, EOS_WX(16), 0);
    lv_obj_set_style_pad_bottom(l, EOS_WY(8), 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_USER_1);   /* scroll-effect target */
    return l;
}
