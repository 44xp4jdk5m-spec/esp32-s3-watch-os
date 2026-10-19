/**
 * @file settings_appview.c
 * @brief Settings > App View & Icon Count pages.
 *
 * App View picker (Grid / List), Grid Layout mode (Hex / Vertical), icon
 * gap / distinct icons / entry animation switches, and the icon-count
 * picker.  Built on the sw_list widgets.
 */

#include "settings_ui_internal.h"
#include "sw_list.h"
#include "sw_title.h"
#include "sw_comment.h"
#include "sw_entry.h"
#include "sw_toggle.h"
#include "sw_picker.h"
#include "sw_icons.h"
#include "app_config.h"
#include "app_lang.h"
#include "list_view_menu.h"
#include "honeycomb_menu.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "SETTINGS";

/* ── Grid / List picker callback (idx 0 = grid, 1 = list) ──────────── */
static void _appview_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx == 0) app_menu_set_style(MENU_STYLE_HONEYCOMB);
    else          app_menu_set_style(MENU_STYLE_LIST);
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
    /* Rebuild in place to show/hide the Grid Layout section. */
    settings_ui_refresh();
}

/* ── Distinct Icons switch ─────────────────────────────────────────── */
static void _distinct_icons_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.honeycomb_distinct_icons = on;
    honeycomb_menu_set_distinct_icons(on);
    app_config_save();
    ESP_LOGI(TAG, "Distinct icons: %s", on ? "ON" : "OFF");
    settings_ui_refresh();
}

/* ── Grid Mode (Hex Spiral / Vertical Grid) picker callback ────────── */
static void _grid_mode_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0) return;

    static const honeycomb_layout_mode_t modes[] = {
        HONEYCOMB_MODE_HEX_SPIRAL, HONEYCOMB_MODE_VERTICAL_GRID
    };
    honeycomb_menu_set_layout_mode(modes[idx]);
    g_config.honeycomb_layout_mode = (uint8_t)modes[idx];
    app_config_save();
    /* Reset icon count to default for the new mode. */
    honeycomb_menu_set_icon_count(honeycomb_menu_get_default_icon_count());

    /* Move the tick to the new selection. */
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
    ESP_LOGI(TAG, "Grid mode: %s",
             modes[idx] == HONEYCOMB_MODE_VERTICAL_GRID ? "Vertical" : "Hex");
    settings_ui_refresh();
}

/* ── Entry animation switches (vertical / hex) ─────────────────────── */
static void _vertical_entry_anim_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.honeycomb_vertical_entry_animation = on;
    app_config_save();
    ESP_LOGI(TAG, "Vertical entry animation: %s", on ? "ON" : "OFF");
}

static void _hex_entry_anim_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.honeycomb_hex_entry_animation = on;
    app_config_save();
    ESP_LOGI(TAG, "Hex entry animation: %s", on ? "ON" : "OFF");
}

/* ── Honeycomb gap adjust switch ───────────────────────────────────── */
static void _gap_adjust_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.honeycomb_gap_adjust = on;
    app_config_save();
    ESP_LOGI(TAG, "Honeycomb gap adjust: %s", on ? "ON (+25%)" : "OFF (original)");
}

/* ── Icon Count picker callback ────────────────────────────────────── */
static void _iconcount_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    honeycomb_layout_mode_t hmode = honeycomb_menu_get_layout_mode();
    static const int hex_counts[] = {1, 7, 19, 37, 61, 91, 127};
    static const int vert_counts[] = {10, 24};
    const int *counts = (hmode == HONEYCOMB_MODE_VERTICAL_GRID)
                        ? vert_counts : hex_counts;

    honeycomb_menu_set_icon_count(counts[idx]);
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
    ESP_LOGI(TAG, "Icon count: %d", counts[idx]);
}

/* ====================================================================
 * Page builders
 * ==================================================================== */

static void _build_appview_page(void)
{
    lv_obj_t *list = settings_ui_list();
    menu_style_t cur = app_menu_get_style();

    sw_comment_add(list, "Choose how apps are displayed in the menu");

    /* ── Group: menu style — Grid / List picker ── */
    sw_picker_opt_t view_opts[2] = {
        { app_lang_get("app_view.grid_view"), SW_ICON_HOME },
        { app_lang_get("app_view.list_view"), SW_ICON_LIST },
    };
    int sel_view = (cur == MENU_STYLE_HONEYCOMB) ? 0 : 1;
    lv_obj_t *picker = sw_picker_create(list, view_opts, 2, sel_view,
                                        false, _appview_cb);
    (void)picker;

    /* ── Hex / Vertical mode picker (only when Grid View selected) ── */
    if (cur == MENU_STYLE_HONEYCOMB) {
        sw_list_add_group_gap(list);
        sw_title_add(list, "Grid Layout");

        honeycomb_layout_mode_t hmode = honeycomb_menu_get_layout_mode();

        /* Grid Layout mode — joined picker (unified, black separators). */
        sw_picker_opt_t mode_opts[2] = {
            { "Hex Spiral",    NULL },
            { "Vertical Grid", NULL },
        };
        int sel_mode = (hmode == HONEYCOMB_MODE_VERTICAL_GRID) ? 1 : 0;
        lv_obj_t *mode_picker = sw_picker_create(list, mode_opts, 2,
                                                 sel_mode, false, _grid_mode_cb);
        (void)mode_picker;

        /* ── Group: appearance — icons, gap, animation, count ──── */
        sw_list_add_group_gap(list);
        sw_toggle_add(list, "Distinct Icons",
                      g_config.honeycomb_distinct_icons,
                      _distinct_icons_switch_cb, NULL);
        sw_comment_add(list, "Only show unique icons (no grid repeats)");

        /* ── Icon gap adjust switch ── */
        sw_toggle_add(list, "Tighten icon gap",
                      g_config.honeycomb_gap_adjust,
                      _gap_adjust_switch_cb, NULL);
        sw_comment_add(list, "ON: +25% — gaps shrink with icon size\n"
                            "OFF: original spacing");

        if (hmode == HONEYCOMB_MODE_VERTICAL_GRID) {
            sw_toggle_add(list, "Vertical entry animation",
                          g_config.honeycomb_vertical_entry_animation,
                          _vertical_entry_anim_switch_cb, NULL);
            sw_comment_add(list, "Animate vertical honeycomb rows one row at a time on entry.");
        } else if (hmode == HONEYCOMB_MODE_HEX_SPIRAL) {
            sw_toggle_add(list, "Hex entry animation",
                          g_config.honeycomb_hex_entry_animation,
                          _hex_entry_anim_switch_cb, NULL);
            sw_comment_add(list, "Scale icons + close in from a big circle on entry.");
        }

        /* ── Icon Count entry (only when distinct is OFF) ── */
        if (!g_config.honeycomb_distinct_icons) {
            char cnt_label[32];
            snprintf(cnt_label, sizeof(cnt_label), "%d icons",
                     honeycomb_menu_get_icon_count());
            sw_entry_add(list, cnt_label,
                         settings_ui_go_nav_cb, (void *)(uintptr_t)P_ICONCOUNT);
        }
    }
}

static void _build_iconcount_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Number of icons in the grid");
    if (g_config.honeycomb_distinct_icons) {
        sw_comment_add(list, "Disabled — Distinct Icons is ON");
        return;
    }

    honeycomb_layout_mode_t hmode = honeycomb_menu_get_layout_mode();
    static const int hex_counts[] = {1, 7, 19, 37, 61, 91, 127};
    static const int vert_counts[] = {10, 24};
    const int *counts;
    int count_len;
    if (hmode == HONEYCOMB_MODE_VERTICAL_GRID) {
        counts = vert_counts;
        count_len = (int)(sizeof(vert_counts) / sizeof(vert_counts[0]));
        sw_comment_add(list, "Vertical: only counts with a centered 3-icon row");
    } else {
        counts = hex_counts;
        count_len = (int)(sizeof(hex_counts) / sizeof(hex_counts[0]));
        sw_comment_add(list, "Hex Spiral: complete hexagonal rings");
    }

    int cur_cnt = honeycomb_menu_get_icon_count();

    sw_picker_opt_t opts[count_len];
    char labels[count_len][16];
    int selected = 0;
    for (int ci = 0; ci < count_len; ci++) {
        snprintf(labels[ci], sizeof(labels[ci]), "%d icons", counts[ci]);
        opts[ci].label = labels[ci];
        opts[ci].icon  = NULL;
        if (counts[ci] == cur_cnt) selected = ci;
    }
    lv_obj_t *picker = sw_picker_create(list, opts, count_len, selected,
                                        false, _iconcount_cb);
    (void)picker;
}

void settings_appview_build(page_t p)
{
    switch (p) {
    case P_APPVIEW:   _build_appview_page();   break;
    case P_ICONCOUNT: _build_iconcount_page(); break;
    default:          break;
    }
}
