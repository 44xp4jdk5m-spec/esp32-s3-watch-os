/**
 * @file sw_row.c
 * @brief Shared pressable row-panel primitive (see sw_row.h).
 */

#include "sw_row.h"
#include "sw_icons.h"

/* Row pressed: scale down to 230 / 256 + darken the panel. */
static lv_style_t s_row_pressed;
static bool       s_row_pressed_init;

static void sw_press_style_init(void)
{
    if (!s_row_pressed_init) {
        lv_style_init(&s_row_pressed);
        lv_style_set_transform_scale(&s_row_pressed, 230);
        lv_style_set_bg_color(&s_row_pressed, SW_COLOR_DARK_GREY_1);
        s_row_pressed_init = true;
    }
}

lv_obj_t *sw_row_create(lv_obj_t *list)
{
    sw_press_style_init();

    lv_obj_t *c = lv_obj_create(list);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(c, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(c, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_size(c, lv_pct(100), SW_ROW_H);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);

    /* Apple Watch press scale = 230 (scale down on press). */
    lv_obj_set_style_transform_pivot_x(c, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(c, lv_pct(50), 0);
    lv_obj_add_style(c, &s_row_pressed, LV_STATE_PRESSED);
    /* Scale animates over 150 ms on press. */
    lv_obj_set_style_anim_duration(c, 150, 0);
    return c;
}

lv_obj_t *sw_row_round_icon(lv_obj_t *row, lv_color_t bg, const void *src)
{
    lv_obj_t *r = lv_obj_create(row);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(r, SW_ROUND_ICON, SW_ROUND_ICON);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(r, bg, 0);
    lv_obj_t *icon = lv_label_create(r);
    lv_label_set_text(icon, (const char *)src);
    lv_obj_set_style_text_font(icon, SW_ICON_F, 0);
    lv_obj_set_style_text_color(icon, SW_COLOR_WHITE, 0);
    lv_obj_set_style_translate_y(icon, 2, 0);
    lv_obj_center(icon);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    return r;
}
