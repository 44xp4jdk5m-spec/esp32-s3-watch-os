/**
 * @file sw_toggle.c
 * @brief On/off switch row (see sw_toggle.h).
 */

#include "sw_toggle.h"
#include "sw_row.h"

/* The whole row toggles the switch (like iOS). */
static void sw_toggle_row_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_user_data(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED))
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    else
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

static lv_obj_t *sw_toggle_impl(lv_obj_t *list, const sw_toggle_cfg_t *cfg,
                                lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = sw_row_create(list);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(c, EOS_WX(16), 0);
    lv_obj_set_style_pad_right(c, EOS_WX(16), 0);

    /* Optional leading round icon. */
    if (cfg->icon) {
        sw_row_round_icon(c, cfg->icon_bg, cfg->icon);
    }

    lv_obj_t *label = lv_label_create(c);
    lv_obj_set_style_text_font(label, SW_FONT_M, 0);
    lv_label_set_text(label, cfg->label);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_color(label, SW_COLOR_WHITE, 0);
    if (cfg->icon) lv_obj_set_style_margin_left(label, EOS_WX(14), 0);

    lv_obj_t *sw = lv_switch_create(c);
    lv_obj_set_size(sw, SW_SWITCH_W, SW_SWITCH_H);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);  /* row toggles it */

    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, SW_COLOR_SWITCH_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, cfg->on_color,
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, SW_COLOR_WHITE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, SW_COLOR_WHITE, LV_PART_KNOB | LV_STATE_CHECKED);
    /* Explicit knob size — smaller than the grey track. */
    lv_obj_set_style_width(sw, EOS_WX(16), LV_PART_KNOB);
    lv_obj_set_style_height(sw, EOS_WY(16), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sw, LV_OPA_30, LV_PART_KNOB);

    if (cfg->checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(c, sw_toggle_row_cb, LV_EVENT_CLICKED, sw);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, ud);
    return sw;
}

lv_obj_t *sw_toggle_add(lv_obj_t *list, const char *label, bool checked,
                        lv_event_cb_t cb, void *ud)
{
    sw_toggle_cfg_t cfg = {
        .label = label, .icon = NULL,
        .on_color = SW_COLOR_GREEN, .checked = checked,
    };
    return sw_toggle_impl(list, &cfg, cb, ud);
}

lv_obj_t *sw_toggle_add_ex(lv_obj_t *list, const sw_toggle_cfg_t *cfg,
                           lv_event_cb_t cb, void *ud)
{
    if (!cfg) return NULL;
    sw_toggle_cfg_t c = *cfg;
    if (c.label == NULL) c.label = "";
    return sw_toggle_impl(list, &c, cb, ud);
}
