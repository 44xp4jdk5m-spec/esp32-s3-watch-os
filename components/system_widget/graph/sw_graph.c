/**
 * @file sw_graph.c
 * @brief Bar-graph + arc-gauge cards (see sw_graph.h).
 *
 * Ported from the settings battery page: identical look, sizes derived
 * from sw_theme.h so the chart scales with the panel.
 */

#include "sw_graph.h"

void sw_graph_arc_gauge(lv_obj_t *list, int pct, lv_obj_t **out_arc,
                        lv_obj_t **out_pct_label)
{
    if (out_arc) *out_arc = NULL;
    if (out_pct_label) *out_pct_label = NULL;

    lv_obj_t *card = lv_obj_create(list);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(card, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_size(card, lv_pct(100), EOS_WY(110));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_USER_1);

    /* Arc on the left side. */
    lv_obj_t *arc = lv_arc_create(card);
    lv_obj_set_size(arc, SW_ARC_SIZE, SW_ARC_SIZE);
    lv_obj_set_style_pad_all(arc, EOS_WY(2), 0);
    lv_obj_set_style_pad_left(arc, EOS_WX(10), 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, pct);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_arc_color(arc, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, EOS_WY(10), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);

    lv_obj_set_style_arc_color(arc, SW_COLOR_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, EOS_WY(10), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

    /* Big % label next to the arc. */
    lv_obj_t *pct_label = lv_label_create(card);
    lv_label_set_text_fmt(pct_label, "%d%%", pct);
    lv_obj_set_style_text_font(pct_label, SW_FONT_L, 0);
    lv_obj_set_style_text_color(pct_label, SW_COLOR_WHITE, 0);
    lv_obj_set_style_pad_left(pct_label, EOS_WX(6), 0);

    if (out_arc) *out_arc = arc;
    if (out_pct_label) *out_pct_label = pct_label;
}

void sw_graph_bar_card(lv_obj_t *list, const uint8_t *pcts,
                       const bool *charging, int count, int max_slots,
                       int current_hour)
{
    /* Outer card container. */
    lv_obj_t *card = lv_obj_create(list);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(card, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, EOS_WX(10), 0);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(card, LV_OBJ_FLAG_USER_1);

    /* ── Chart area: Y axis + bars ── */
    lv_obj_t *chart_row = lv_obj_create(card);
    lv_obj_remove_flag(chart_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chart_row, lv_pct(100), SW_GRAPH_H);
    lv_obj_set_style_bg_opa(chart_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart_row, 0, 0);
    lv_obj_set_style_pad_all(chart_row, 0, 0);
    lv_obj_set_flex_flow(chart_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chart_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Y axis labels (100% / 0%). */
    lv_obj_t *y_axis = lv_obj_create(chart_row);
    lv_obj_remove_flag(y_axis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(y_axis, EOS_WX(36), SW_GRAPH_H);
    lv_obj_set_style_bg_opa(y_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(y_axis, 0, 0);
    lv_obj_set_style_pad_all(y_axis, 0, 0);
    lv_obj_set_flex_flow(y_axis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(y_axis, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *yl100 = lv_label_create(y_axis);
    lv_label_set_text(yl100, "100%");
    lv_obj_set_style_text_font(yl100, SW_FONT_S, 0);
    lv_obj_set_style_text_color(yl100, SW_COLOR_GREY, 0);

    lv_obj_t *yl0 = lv_label_create(y_axis);
    lv_label_set_text(yl0, "0%");
    lv_obj_set_style_text_font(yl0, SW_FONT_S, 0);
    lv_obj_set_style_text_color(yl0, SW_COLOR_GREY, 0);

    /* Bars container with bottom border. */
    lv_obj_t *bars_ctn = lv_obj_create(chart_row);
    lv_obj_remove_flag(bars_ctn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(bars_ctn, 1);
    lv_obj_set_style_bg_opa(bars_ctn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bars_ctn, 0, 0);
    lv_obj_set_style_pad_all(bars_ctn, 0, 0);
    lv_obj_set_style_radius(bars_ctn, 0, 0);
    lv_obj_set_size(bars_ctn, lv_pct(100), SW_GRAPH_H);
    lv_obj_set_flex_flow(bars_ctn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars_ctn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);

    /* 50% grid line as a floating overlay. */
    lv_obj_t *grid_line = lv_obj_create(chart_row);
    lv_obj_add_flag(grid_line, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(grid_line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(grid_line, SW_COLOR_SEP, 0);
    lv_obj_set_style_bg_opa(grid_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(grid_line, 0, 0);
    lv_obj_set_style_pad_all(grid_line, 0, 0);
    lv_obj_set_style_radius(grid_line, 0, 0);
    lv_obj_set_pos(grid_line, 0, SW_GRAPH_H / 2);

    /* Render bars — newest (current reading) pinned to the RIGHT edge. */
    int data_count = count;
    if (data_count > max_slots) data_count = max_slots;
    int bars = (data_count > 0) ? data_count : 1;   /* at least current */
    bool use_current = (data_count == 0);
    int leading = max_slots - bars;                 /* right-align bars */

    for (int i = 0; i < leading; i++) {
        lv_obj_t *sp = lv_obj_create(bars_ctn);
        lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_grow(sp, 1);
        lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
        lv_obj_set_style_pad_all(sp, 0, 0);
        lv_obj_set_height(sp, 1);
    }

    for (int i = 0; i < bars; i++) {
        uint8_t pct;
        bool is_chg;
        if (use_current) {
            pct = (uint8_t)(pcts ? pcts[0] : 0);
            is_chg = charging ? charging[0] : false;
        } else {
            pct = pcts ? pcts[i] : 0;
            is_chg = charging ? charging[i] : false;
        }

        lv_obj_t *bar = lv_obj_create(bars_ctn);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_radius(bar, EOS_WX(2), 0);
        lv_obj_set_width(bar, EOS_WX(8));
        /* 100% battery = full chart height. */
        lv_obj_set_height(bar, (lv_coord_t)((pct * SW_GRAPH_H) / 100));

        lv_color_t bar_color = is_chg ? SW_COLOR_ORANGE : SW_COLOR_GREEN;
        lv_obj_set_style_bg_color(bar, bar_color, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        /* Spacer between bars. */
        if (i < bars - 1) {
            lv_obj_t *spacer = lv_obj_create(bars_ctn);
            lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_grow(spacer, 1);
            lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(spacer, 0, 0);
            lv_obj_set_style_pad_all(spacer, 0, 0);
            lv_obj_set_height(spacer, 1);
        }
    }

    /* ── X axis labels (12AM / 6AM / 12PM / 6PM) ── */
    lv_obj_t *x_axis = lv_obj_create(card);
    lv_obj_remove_flag(x_axis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(x_axis, lv_pct(100), EOS_WY(20));
    lv_obj_set_style_bg_opa(x_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(x_axis, 0, 0);
    lv_obj_set_style_pad_all(x_axis, 0, 0);
    lv_obj_set_style_pad_left(x_axis, EOS_WX(36), 0);
    lv_obj_set_flex_flow(x_axis, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(x_axis, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* The x-axis is a 24 h window ending at "now"; 4 labels are the 6 h
     * grid ticks inside it, ordered oldest → newest. */
    static const char *base_x_labels[] = {"12AM", "6AM", "12PM", "6PM"};
    int last_tick = current_hour - (current_hour % 6);
    for (int i = 0; i < 4; i++) {
        int idx = ((last_tick / 6) - 3 + i) & 3;
        lv_obj_t *xl = lv_label_create(x_axis);
        lv_label_set_text(xl, base_x_labels[idx]);
        lv_obj_set_style_text_font(xl, SW_FONT_S, 0);
        lv_obj_set_style_text_color(xl, SW_COLOR_GREY, 0);
    }
}

/* ====================================================================
 * Weekly stacked screen-time graph (7 trailing calendar days)
 * ==================================================================== */

void sw_graph_week_card(lv_obj_t *list, const sw_week_day_t *days,
                        const char *const *letters, uint32_t avg_sec)
{
    /* Outer rounded card (same look as the battery graph card). */
    lv_obj_t *card = lv_obj_create(list);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(card, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, EOS_WX(10), 0);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(card, LV_OBJ_FLAG_USER_1);

    /* Overall max day total (scale denominator). */
    uint32_t max = 1;
    for (int d = 0; d < 7; d++) {
        uint32_t t = days[d].social + days[d].productivity +
                     days[d].other + days[d].system;
        if (t > max) max = t;
    }

    /* Chart area: 7 equal, contiguous day stacks. */
    lv_obj_t *chart_row = lv_obj_create(card);
    lv_obj_remove_flag(chart_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chart_row, lv_pct(100), SW_GRAPH_H);
    lv_obj_set_style_bg_opa(chart_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart_row, 0, 0);
    lv_obj_set_style_pad_all(chart_row, 0, 0);
    lv_obj_set_flex_flow(chart_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chart_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);

    for (int d = 0; d < 7; d++) {
        lv_obj_t *stack = lv_obj_create(chart_row);
        lv_obj_remove_flag(stack, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(stack, 0, 0);
        lv_obj_set_style_pad_all(stack, 0, 0);
        lv_obj_set_style_radius(stack, EOS_WX(3), 0);
        lv_obj_set_style_clip_corner(stack, true, 0);
        lv_obj_set_style_bg_opa(stack, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_grow(stack, 1);
        lv_obj_set_height(stack, SW_GRAPH_H);
        /* Stack from the bottom: first child = bottom (social). */
        lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN_REVERSE);
        lv_obj_set_flex_align(stack, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        uint32_t  seg[4] = { days[d].social, days[d].productivity,
                             days[d].other, days[d].system };
        lv_color_t col[4] = { SW_COLOR_BLUE, SW_COLOR_LIGHT_BLUE,
                              SW_COLOR_ORANGE, SW_COLOR_GREY };
        for (int c = 0; c < 4; c++) {
            if (seg[c] == 0) continue;
            lv_obj_t *s = lv_obj_create(stack);
            lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_border_width(s, 0, 0);
            lv_obj_set_style_pad_all(s, 0, 0);
            lv_obj_set_style_radius(s, 0, 0);
            lv_obj_set_width(s, lv_pct(100));
            lv_obj_set_height(s, (lv_coord_t)((seg[c] * SW_GRAPH_H) / max));
            lv_obj_set_style_bg_color(s, col[c], 0);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        }
    }

    /* Green dotted average line (floating over the bars). */
    if (avg_sec > 0 && avg_sec <= max) {
        lv_obj_t *avg = lv_obj_create(chart_row);
        lv_obj_add_flag(avg, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(avg, lv_pct(100), 2);
        lv_obj_set_style_bg_opa(avg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(avg, 0, 0);
        lv_obj_set_style_pad_all(avg, 0, 0);
        lv_obj_set_pos(avg, 0,
                       (lv_coord_t)(SW_GRAPH_H - (avg_sec * SW_GRAPH_H) / max));
        lv_obj_set_flex_flow(avg, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(avg, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        for (int i = 0; i < 18; i++) {
            lv_obj_t *dot = lv_obj_create(avg);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_pad_all(dot, 0, 0);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_size(dot, EOS_WX(2), 2);
            lv_obj_set_style_bg_color(dot, SW_COLOR_GREEN, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        }
    }

    /* X-axis weekday letters. */
    lv_obj_t *x_axis = lv_obj_create(card);
    lv_obj_remove_flag(x_axis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(x_axis, lv_pct(100), EOS_WY(20));
    lv_obj_set_style_bg_opa(x_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(x_axis, 0, 0);
    lv_obj_set_style_pad_all(x_axis, 0, 0);
    lv_obj_set_flex_flow(x_axis, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(x_axis, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    for (int d = 0; d < 7; d++) {
        lv_obj_t *xl = lv_label_create(x_axis);
        lv_label_set_text(xl, letters[d]);
        lv_obj_set_style_text_font(xl, SW_FONT_S, 0);
        lv_obj_set_style_text_align(xl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_flex_grow(xl, 1);
        lv_obj_set_style_text_color(xl, SW_COLOR_GREY, 0);
    }
}
