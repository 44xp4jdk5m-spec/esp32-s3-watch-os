/**
 * @file sw_picker.c
 * @brief Vertical radio + horizontal segmented pickers (see sw_picker.h).
 *
 * All separators are SW_COLOR_BLACK hairline lines so every picker looks
 * consistent (previously some callers passed a light-grey separator).
 */

#include "sw_picker.h"
#include "sw_icons.h"

/* Picker option pressed: darken the panel. */
static lv_style_t s_picker_pressed;
static bool       s_picker_pressed_init;

static void sw_picker_press_init(void)
{
    if (!s_picker_pressed_init) {
        lv_style_init(&s_picker_pressed);
        lv_style_set_bg_color(&s_picker_pressed,
                              lv_color_darken(SW_THEME_SECONDARY, 64));
        lv_style_set_bg_opa(&s_picker_pressed, LV_OPA_COVER);
        lv_style_set_anim_duration(&s_picker_pressed, 150);
        s_picker_pressed_init = true;
    }
}

/* Config stored in the picker object's user_data. */
typedef struct {
    lv_event_cb_t sel_cb;
    bool          segmented;
    int           opt_count;
} sw_picker_cfg_t;

static void sw_picker_delete_cb(lv_event_t *e)
{
    sw_picker_cfg_t *cfg = lv_event_get_user_data(e);
    if (cfg) lv_free(cfg);
}

/* ====================================================================
 * Vertical joined picker
 * ==================================================================== */

lv_obj_t *sw_picker_create(lv_obj_t *list, const sw_picker_opt_t *opts,
                           int count, int selected, bool tick_left,
                           lv_event_cb_t sel_cb)
{
    sw_picker_press_init();

    lv_obj_t *picker = lv_obj_create(list);
    lv_obj_remove_flag(picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(picker, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(picker, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(picker, 0, 0);
    lv_obj_set_style_pad_all(picker, 0, 0);
    lv_obj_set_style_clip_corner(picker, true, 0);
    lv_obj_set_size(picker, lv_pct(100), SW_ROW_H * count);
    lv_obj_set_flex_flow(picker, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(picker, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    sw_picker_cfg_t *cfg = lv_malloc(sizeof(sw_picker_cfg_t));
    if (cfg) {
        cfg->sel_cb    = sel_cb;
        cfg->segmented = false;
        cfg->opt_count = count;
        lv_obj_set_user_data(picker, cfg);
        lv_obj_add_event_cb(picker, sw_picker_delete_cb, LV_EVENT_DELETE, cfg);
    }

    for (int i = 0; i < count; i++) {
        const sw_picker_opt_t *opt = &opts[i];

        lv_obj_t *btn = lv_obj_create(picker);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(btn, lv_pct(100), SW_ROW_H);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_style(btn, &s_picker_pressed, LV_STATE_PRESSED);

        /* Left-side tick (radio style). */
        if (tick_left) {
            lv_obj_t *tick = lv_label_create(btn);
            lv_label_set_text(tick, SW_ICON_CHECK);
            lv_obj_set_style_text_font(tick, SW_ICON_F, 0);
            lv_obj_set_style_text_color(tick, SW_THEME_PRIMARY, 0);
            lv_obj_set_style_pad_left(tick, EOS_WX(16), 0);
            if (i != selected) lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tick, LV_OBJ_FLAG_USER_2);   /* tick marker */
        }

        /* Leading icon (optional). */
        if (opt->icon) {
            lv_obj_t *ic = lv_label_create(btn);
            lv_label_set_text(ic, (const char *)opt->icon);
            lv_obj_set_style_text_font(ic, SW_ICON_F, 0);
            lv_obj_set_style_text_color(ic, SW_COLOR_WHITE, 0);
            lv_obj_set_style_pad_left(ic, EOS_WX(16), 0);
        }

        /* Option label. */
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, SW_FONT_M, 0);
        lv_label_set_text(lbl, opt->label);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_flex_grow(lbl, 1);
        lv_obj_set_style_text_color(lbl, SW_COLOR_WHITE, 0);
        lv_obj_set_style_pad_left(lbl, tick_left ? EOS_WX(14) : EOS_WX(16), 0);

        /* Right-side check tick. */
        if (!tick_left) {
            lv_obj_t *tick = lv_label_create(btn);
            lv_label_set_text(tick, SW_ICON_CHECK);
            lv_obj_set_style_text_font(tick, SW_ICON_F, 0);
            lv_obj_set_style_text_color(tick, SW_THEME_PRIMARY, 0);
            lv_obj_set_style_pad_right(tick, EOS_WX(16), 0);
            if (i != selected) lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(tick, LV_OBJ_FLAG_USER_2);   /* tick marker */
        }

        /* Black hairline separator between options. */
        if (i < count - 1) {
            lv_obj_t *sep = lv_obj_create(picker);
            lv_obj_set_size(sep, lv_pct(100), 1);
            lv_obj_set_style_bg_color(sep, SW_COLOR_BLACK, 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(sep, 0, 0);
            lv_obj_set_style_pad_all(sep, 0, 0);
            lv_obj_set_style_radius(sep, 0, 0);
        }

        /* The shared selection callback receives the option index. */
        if (cfg && cfg->sel_cb) {
            lv_obj_add_event_cb(btn, cfg->sel_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)i);
        }
    }
    return picker;
}

/* ====================================================================
 * Horizontal segmented picker
 * ==================================================================== */

lv_obj_t *sw_picker_create_segmented(lv_obj_t *list,
                                     const sw_picker_opt_t *opts,
                                     int count, int selected,
                                     lv_event_cb_t sel_cb)
{
    sw_picker_press_init();

    lv_obj_t *picker = lv_obj_create(list);
    lv_obj_remove_flag(picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(picker, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(picker, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(picker, 0, 0);
    lv_obj_set_style_pad_all(picker, 0, 0);
    lv_obj_set_style_clip_corner(picker, true, 0);
    lv_obj_set_size(picker, lv_pct(100), SW_ROW_H);
    lv_obj_set_flex_flow(picker, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(picker, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    sw_picker_cfg_t *cfg = lv_malloc(sizeof(sw_picker_cfg_t));
    if (cfg) {
        cfg->sel_cb    = sel_cb;
        cfg->segmented = true;
        cfg->opt_count = count;
        lv_obj_set_user_data(picker, cfg);
        lv_obj_add_event_cb(picker, sw_picker_delete_cb, LV_EVENT_DELETE, cfg);
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t *btn = lv_obj_create(picker);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(btn, lv_pct(100) / count, SW_ROW_H);
        lv_obj_set_style_flex_grow(btn, 1, 0);
        lv_obj_set_style_bg_color(btn, (i == selected) ? SW_THEME_PRIMARY
                                                       : SW_THEME_SECONDARY, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_style(btn, &s_picker_pressed, LV_STATE_PRESSED);
        /* Identify option buttons for set_selected(). */
        lv_obj_add_flag(btn, LV_OBJ_FLAG_USER_3);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, opts[i].label);
        lv_obj_set_style_text_font(lbl, SW_FONT_M, 0);
        lv_obj_set_style_text_color(lbl, SW_COLOR_WHITE, 0);

        /* Hairline vertical separator (black, like the vertical picker). */
        if (i < count - 1) {
            lv_obj_t *sep = lv_obj_create(picker);
            lv_obj_set_size(sep, 1, lv_pct(70));
            lv_obj_set_style_bg_color(sep, SW_COLOR_BLACK, 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(sep, 0, 0);
            lv_obj_set_style_pad_all(sep, 0, 0);
            lv_obj_set_style_radius(sep, 0, 0);
        }

        if (cfg && cfg->sel_cb) {
            lv_obj_add_event_cb(btn, cfg->sel_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)i);
        }
    }
    return picker;
}

/* ====================================================================
 * Set selected
 * ==================================================================== */

void sw_picker_set_selected(lv_obj_t *picker, int idx)
{
    sw_picker_cfg_t *cfg = lv_obj_get_user_data(picker);
    if (!cfg) return;

    if (cfg->segmented) {
        /* Option buttons carry LV_OBJ_FLAG_USER_3. */
        int n = 0;
        uint32_t child_count = lv_obj_get_child_count(picker);
        for (uint32_t j = 0; j < child_count; j++) {
            lv_obj_t *c = lv_obj_get_child(picker, j);
            if (!c || !lv_obj_has_flag(c, LV_OBJ_FLAG_USER_3)) continue;
            lv_obj_set_style_bg_color(c,
                (n == idx) ? SW_THEME_PRIMARY : SW_THEME_SECONDARY, 0);
            n++;
        }
        return;
    }

    /* Vertical picker: buttons at even child indices, separators at odd
     * ones.  Each button's check tick is marked with LV_OBJ_FLAG_USER_2. */
    uint32_t n = lv_obj_get_child_count(picker);
    for (uint32_t j = 0; j < n; j++) {
        lv_obj_t *c = lv_obj_get_child(picker, j);
        if (!c || (j & 1)) continue;              /* skip separators */
        uint32_t cc = lv_obj_get_child_count(c);
        for (uint32_t k = 0; k < cc; k++) {
            lv_obj_t *tick = lv_obj_get_child(c, k);
            if (!lv_obj_has_flag(tick, LV_OBJ_FLAG_USER_2)) continue;
            bool on = ((int)(j / 2) == idx);
            if (on) lv_obj_remove_flag(tick, LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
            break;
        }
    }
}
