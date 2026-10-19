/**
 * @file sw_info.c
 * @brief Auto-height key/value panel (see sw_info.h).
 *
 * Layout: rounded SW_THEME_SECONDARY panel, ROW flex, natural height from
 * the tallest child + vertical padding.  Label (SW_FONT_M, white) grows to
 * fill; an optional value label sits on the right (default grey SW_FONT_S).
 */

#include "sw_info.h"

lv_obj_t *sw_info_add(lv_obj_t *list, const char *label,
                      const char *value, lv_color_t value_color,
                      const lv_font_t *value_font)
{
    lv_obj_t *p = lv_obj_create(list);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(p, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(p, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_hor(p, EOS_WX(16), 0);
    lv_obj_set_style_pad_top(p, EOS_WY(12), 0);
    lv_obj_set_style_pad_bottom(p, EOS_WY(12), 0);
    /* Height follows the text (labels are not fixed 100 px rows). */
    lv_obj_set_size(p, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(p, LV_OBJ_FLAG_USER_1);   /* scroll-effect target */

    lv_obj_t *lbl = lv_label_create(p);
    lv_obj_set_style_text_font(lbl, SW_FONT_M, 0);
    lv_label_set_text(lbl, label);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_flex_grow(lbl, 1);
    lv_obj_set_style_text_color(lbl, SW_COLOR_WHITE, 0);

    if (value) {
        lv_obj_t *val = lv_label_create(p);
        lv_label_set_text(val, value);
        lv_label_set_long_mode(val, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_font(val, value_font ? value_font : SW_FONT_S, 0);
        lv_obj_set_style_text_color(val, value_color, 0);
        lv_obj_set_style_pad_left(val, EOS_WX(8), 0);
        lv_obj_set_style_pad_right(val, EOS_WX(0), 0);
    }
    return p;
}
