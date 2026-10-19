/**
 * @file sw_button.c
 * @brief Full-width pill action button (see sw_button.h).
 */

#include "sw_button.h"

lv_obj_t *sw_button_add(lv_obj_t *list, const char *txt,
                        lv_color_t txt_color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(list);
    lv_obj_set_size(b, lv_pct(100), SW_ROW_H);
    lv_obj_set_style_bg_color(b, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(b, LV_DIR_NONE);
    /* Scale from the centre so press/transition scaling never leaves
     * an asymmetric gap on the right. */
    lv_obj_set_style_transform_pivot_x(b, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(b, lv_pct(50), 0);
    /* Flex-centre so callers can append a spinner next to the label. */
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, SW_FONT_M, 0);
    lv_obj_set_style_text_color(l, txt_color, 0);

    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}
