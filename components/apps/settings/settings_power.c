/**
 * @file settings_power.c
 * @brief Settings > Power pages + battery history SPIFFS storage.
 *
 * Contains: power overview (CPU protection + temperature thresholds),
 * battery page (arc gauge, 24 h bar graph, "last charged", charge limit,
 * low power mode), screen-time stats, and the battery_history_* storage
 * helpers (public API declared in settings_ui.h).
 */

#include "settings_ui_internal.h"
#include "battery_history.h"
#include "sw_list.h"
#include "sw_title.h"
#include "sw_comment.h"
#include "sw_icon_btn.h"
#include "sw_entry.h"
#include "sw_toggle.h"
#include "sw_picker.h"
#include "sw_info.h"
#include "sw_graph.h"
#include "sw_icons.h"
#include "app_config.h"
#include "app_events.h"             /* g_battery_pct, g_is_charging, temps */
#include "app_power.h"
#include "app_lang.h"
#include "hw_rtc.h"
#include "app_registry.h"
#include "app_usage.h"
#include "power_stats.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "SETTINGS";

/* ====================================================================
 * Power page callbacks
 * ==================================================================== */

static void _charge_limit_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_config.charge_limit_80 = lv_obj_has_state(sw, LV_STATE_CHECKED);
    app_config_save();
    ESP_LOGI(TAG, "Charge limit 80%%: %s",
             g_config.charge_limit_80 ? "ON" : "OFF");
    /* Show/hide the Optimized Charging row. */
    settings_ui_refresh();
}

static void _optimized_charge_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_config.optimized_charging_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    app_config_save();
    ESP_LOGI(TAG, "Optimized Charging: %s",
             g_config.optimized_charging_enabled ? "ON" : "OFF");
}

static void _low_power_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    /* Shared Low Power apply (brightness, dim timeout, 15 FPS cap, anim
     * kill) — used by both the Settings switch and the control center. */
    app_power_set_low_power(on);
}

static void _cpu_prot_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.cpu_protection_enabled = on;
    app_config_save();
    ESP_LOGI(TAG, "CPU Protection: %s", on ? "ON" : "OFF");
    settings_ui_refresh();
}

/* ── Temperature picker (shared by chips & cpu pages) ──────────────── */
static const uint8_t _temp_opts[] = {30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80};
#define _TEMP_OPT_COUNT  (sizeof(_temp_opts) / sizeof(_temp_opts[0]))

static void _temp_chips_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)_TEMP_OPT_COUNT) return;
    g_config.power_chips_temp_threshold = _temp_opts[idx];
    app_config_save();
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
}

static void _temp_cpu_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)_TEMP_OPT_COUNT) return;
    g_config.cpu_temp_threshold = _temp_opts[idx];
    app_config_save();
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
}

static void _build_temp_picker(uint8_t cur_val, lv_event_cb_t cb,
                               const char *desc)
{
    lv_obj_t *list = settings_ui_list();
    sw_comment_add(list, desc);

    sw_picker_opt_t opts[_TEMP_OPT_COUNT];
    char labels[_TEMP_OPT_COUNT][16];
    int selected = 0;
    for (int i = 0; i < (int)_TEMP_OPT_COUNT; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%d °C", _temp_opts[i]);
        opts[i].label = labels[i];
        opts[i].icon  = NULL;
        if (_temp_opts[i] == cur_val) selected = i;
    }
    lv_obj_t *picker = sw_picker_create(list, opts, (int)_TEMP_OPT_COUNT,
                                        selected, false, cb);
    (void)picker;
}

/* ====================================================================
 * Live-update handles (dynamic values refreshed while a page is shown)
 * ==================================================================== */
static lv_obj_t *s_batt_arc       = NULL;
static lv_obj_t *s_batt_pct       = NULL;
static int       s_batt_pct_shown = -1;
static lv_obj_t *s_power_temp     = NULL;
static float     s_power_cpu_shown  = -99.0f;
static float     s_power_pmic_shown = -99.0f;
static lv_obj_t *s_st_today       = NULL;
static lv_obj_t *s_st_week_val    = NULL;
static uint32_t  s_st_today_shown = 0;
static uint32_t  s_st_week_shown  = 0;

static void live_reset_all(void)
{
    s_batt_arc       = NULL;
    s_batt_pct       = NULL;
    s_batt_pct_shown = -1;
    s_power_temp     = NULL;
    s_power_cpu_shown  = -99.0f;
    s_power_pmic_shown = -99.0f;
    s_st_today       = NULL;
    s_st_week_val    = NULL;
    s_st_today_shown = 0;
    s_st_week_shown  = 0;
}

/* ====================================================================
 * Shared helpers (duration format / weekday letters / app usage rows)
 * ==================================================================== */

static void _fmt_dur(char *buf, size_t sz, uint32_t sec)
{
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    snprintf(buf, sz, "%uh %02um", h, m);
}

static const char *weekday_letter(int wday)   /* tm_wday: 0 = Sunday */
{
    static const char *const L[7] = { "S", "M", "T", "W", "T", "F", "S" };
    if (wday < 0 || wday > 6) return "?";
    return L[wday];
}

/* Fill the 7 trailing calendar days (oldest → today) + weekday letters. */
static void screen_time_week(sw_week_day_t days[7], const char *letters[7],
                             uint32_t *avg_sec)
{
    int twd = -1;
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) == ESP_OK) {
        time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
        hw_rtc_time_t local;
        hw_rtc_from_unix(ts, &local);
        twd = local.wday;
    }
    for (int d = 0; d < 7; d++) {
        int wd = (twd - (6 - d)) % 7;          /* oldest … today */
        if (wd < 0) wd += 7;
        letters[d] = weekday_letter(wd);
        int ago = 6 - d;                       /* 0 = today … 6 = oldest */
        days[d].social       = app_usage_cat_sec(ago, APP_USAGE_CAT_SOCIAL);
        days[d].productivity = app_usage_cat_sec(ago, APP_USAGE_CAT_PRODUCTIVITY);
        days[d].other        = app_usage_cat_sec(ago, APP_USAGE_CAT_OTHER);
        days[d].system       = app_usage_cat_sec(ago, APP_USAGE_CAT_SYSTEM);
    }
    *avg_sec = app_usage_week_total_sec() / APP_USAGE_TRACK_DAYS;
}

/* Circular app icon (matches the Settings > Apps list style). */
static void _usage_circle_image(lv_obj_t *parent, const lv_image_dsc_t *img,
                                lv_coord_t size)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    if (img && img->header.w > 0 && img->header.h > 0) {
        lv_obj_t *im = lv_image_create(c);
        lv_image_set_src(im, img);
        lv_image_set_inner_align(im, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_size(im, size, size);
        uint32_t sx = ((uint32_t)size << 8) / img->header.w;
        uint32_t sy = ((uint32_t)size << 8) / img->header.h;
        uint32_t s  = (sx > sy) ? sx : sy;
        lv_image_set_scale_x(im, s);
        lv_image_set_scale_y(im, s);
        lv_obj_center(im);
    } else {
        lv_obj_t *icon = lv_label_create(c);
        lv_label_set_text(icon, SW_ICON_APPS);
        lv_obj_set_style_text_font(icon, SW_ICON_F, 0);
        lv_obj_set_style_text_color(icon, SW_COLOR_WHITE, 0);
        lv_obj_center(icon);
    }
}

/* One per-app usage row: circular icon + name, with the usage bar under the
 * name and the exact time (xh xm) aligned to the bar's right end. */
static void _usage_row(lv_obj_t *list, const app_entry_t *a,
                       uint32_t sec, uint32_t max_sec)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_radius(row, SW_LIST_RADIUS, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, EOS_WX(16), 0);
    lv_obj_set_style_pad_ver(row, EOS_WY(12), 0);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_USER_1);

    _usage_circle_image(row, a ? a->icon : NULL, EOS_WX(56));

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_radius(col, 0, 0);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_left(col, EOS_WX(12), 0);

    lv_obj_t *name = lv_label_create(col);
    lv_label_set_text(name, (a && a->name) ? a->name : "?");
    lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(name, SW_FONT_M, 0);
    lv_obj_set_style_text_color(name, SW_COLOR_WHITE, 0);

    lv_obj_t *sub = lv_obj_create(col);
    lv_obj_remove_flag(sub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(sub, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sub, 0, 0);
    lv_obj_set_style_pad_all(sub, 0, 0);
    lv_obj_set_style_radius(sub, 0, 0);
    lv_obj_set_size(sub, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(sub, EOS_WY(6), 0);
    lv_obj_set_flex_flow(sub, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sub, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *track = lv_obj_create(sub);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(track, 1);
    lv_obj_set_height(track, EOS_WY(8));
    lv_obj_set_style_bg_opa(track, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);

    uint32_t frac = (max_sec > 0 && sec > 0)
                    ? (uint32_t)(((uint64_t)sec * 100u) / max_sec) : 0;
    if (frac > 100) frac = 100;
    lv_obj_t *bar = lv_obj_create(track);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_width(bar, lv_pct((int32_t)frac));
    lv_obj_set_height(bar, EOS_WY(8));
    lv_obj_set_style_bg_color(bar, SW_COLOR_SYSTEM_GREY, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    char tbuf[16];
    _fmt_dur(tbuf, sizeof(tbuf), sec);
    lv_obj_t *tl = lv_label_create(sub);
    lv_label_set_text(tl, tbuf);
    lv_obj_set_style_text_font(tl, SW_FONT_S, 0);
    lv_obj_set_style_text_color(tl, SW_COLOR_TEXT_GREY, 0);
    lv_obj_set_style_pad_left(tl, EOS_WX(8), 0);
}

/* ====================================================================
 * Page builders
 * ==================================================================== */

static void _build_power_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_icon_btn_add(list, SW_COLOR_GREEN, SW_ICON_POWER,
                    app_lang_get("settings.battery"),
                    settings_ui_go_nav_cb, (void *)(uintptr_t)P_BATTERY_PAGE);
    sw_icon_btn_add(list, SW_COLOR_PURPLE, SW_ICON_SCHEDULE, "Screen Time",
                    settings_ui_go_nav_cb, (void *)(uintptr_t)P_SCREEN_TIME);
    sw_list_add_group_gap(list);

    sw_toggle_add(list, app_lang_get("power.cpu_protection"),
                  g_config.cpu_protection_enabled,
                  _cpu_prot_switch_cb, NULL);

    /* Description: show current temperatures. */
    char temp_desc[128];
    int off = snprintf(temp_desc, sizeof(temp_desc), "%s",
                       app_lang_get("power.cpu_protection_desc"));
    if (g_cpu_temperature >= 0.0f || g_pmic_temperature >= 0.0f) {
        off += snprintf(temp_desc + off, sizeof(temp_desc) - off, "\nCurrent:");
        if (g_cpu_temperature >= 0.0f) {
            off += snprintf(temp_desc + off, sizeof(temp_desc) - off,
                            "  CPU %.1f°C", g_cpu_temperature);
        }
        if (g_pmic_temperature >= 0.0f) {
            off += snprintf(temp_desc + off, sizeof(temp_desc) - off,
                            "  PMIC %.1f°C", g_pmic_temperature);
        }
    }
    s_power_temp = sw_comment_add(list, temp_desc);
    s_power_cpu_shown  = g_cpu_temperature;
    s_power_pmic_shown = g_pmic_temperature;

    if (g_config.cpu_protection_enabled) {
        char chips_label[32];
        snprintf(chips_label, sizeof(chips_label), "%s  %d °C",
                 app_lang_get("power.power_chips"),
                 g_config.power_chips_temp_threshold);
        sw_entry_add(list, chips_label,
                     settings_ui_go_nav_cb, (void *)(uintptr_t)P_POWER_CHIPS);
        sw_comment_add(list, app_lang_get("power.chips_temp_threshold_desc"));

        char cpu_label[32];
        snprintf(cpu_label, sizeof(cpu_label), "CPU  %d °C",
                 g_config.cpu_temp_threshold);
        sw_entry_add(list, cpu_label,
                     settings_ui_go_nav_cb, (void *)(uintptr_t)P_POWER_CPU);
        sw_comment_add(list, "ESP32-S3 CPU temperature limit.\n"
                             "When exceeded, WiFi & BLE stop.");
    }
}

static void _build_battery_page(void)
{
    lv_obj_t *list = settings_ui_list();

    /* ── Load history from SPIFFS ────────────────────────────────── */
    battery_history_entry_t batt_hist[BATTERY_HISTORY_MAX];
    int hist_count = battery_history_load(batt_hist);

    /* ── 1. Arc gauge + big live %. ──────────────────────────────── */
    lv_obj_t *arc = NULL, *pct_lbl = NULL;
    sw_graph_arc_gauge(list, g_battery_pct, &arc, &pct_lbl);
    s_batt_arc = arc;
    s_batt_pct = pct_lbl;
    s_batt_pct_shown = g_battery_pct;

    /* ── 2. History bar graph (last 24 h). ───────────────────────── */
    {
        /* Copy history into flat arrays for the graph widget. */
        uint8_t pcts[BATTERY_HISTORY_MAX];
        bool    charging[BATTERY_HISTORY_MAX];
        int     count = hist_count;
        for (int i = 0; i < hist_count; i++) {
            pcts[i]     = batt_hist[i].pct;
            charging[i] = batt_hist[i].is_charging;
        }
        if (count == 0) {           /* at least the current reading */
            pcts[0]     = (uint8_t)g_battery_pct;
            charging[0] = g_is_charging;
            count = 1;
        }

        /* X-axis labels rotate with the current local hour. */
        int current_hour = 0;
        hw_rtc_time_t rt;
        if (hw_rtc_get_time(&rt) == ESP_OK) {
            time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
            hw_rtc_time_t local;
            hw_rtc_from_unix(ts, &local);
            current_hour = local.hour;
        }
        sw_graph_bar_card(list, pcts, charging, count,
                          BATTERY_HISTORY_MAX, current_hour);
    }

    sw_list_add_group_gap(list);

    /* ── 3. "Last Charged to 100%" (auto-height info row) ───────── */
    {
        const char *rel_time = "Never";
        if (g_config.last_charged_to_100_ts > 0) {
            hw_rtc_time_t rt;
            if (hw_rtc_get_time(&rt) == ESP_OK) {
                time_t now = hw_rtc_to_unix(&rt);
                time_t diff = now - g_config.last_charged_to_100_ts;
                if (diff < 0) diff = 0;
                static char rel_buf[32];
                if (diff < 60) {
                    snprintf(rel_buf, sizeof(rel_buf), "1m ago");
                } else if (diff < 3600) {
                    snprintf(rel_buf, sizeof(rel_buf), "%dm ago", (int)(diff / 60));
                } else if (diff < 86400) {
                    snprintf(rel_buf, sizeof(rel_buf), "%dh ago", (int)(diff / 3600));
                } else if (diff < 604800) {
                    snprintf(rel_buf, sizeof(rel_buf), "%dd ago", (int)(diff / 86400));
                } else if (diff < 2592000) {
                    snprintf(rel_buf, sizeof(rel_buf), "%dw ago", (int)(diff / 604800));
                } else if (diff < 31536000) {
                    snprintf(rel_buf, sizeof(rel_buf), "%dm ago", (int)(diff / 2592000));
                } else {
                    snprintf(rel_buf, sizeof(rel_buf), "%dy ago", (int)(diff / 31536000));
                }
                rel_time = rel_buf;
            }
        }
        sw_info_add(list, "Last Charged to 100%", rel_time,
                    SW_COLOR_TEXT_GREY, SW_FONT_S);
    }

    /* ── 3b. Power estimate (moved here from the Screen Time page) ─ */
    if (g_is_charging) {
        sw_info_add(list, "Average Drain", "USB charging",
                    SW_COLOR_TEXT_GREY, SW_FONT_S);
    } else {
        float avg_ma = 0.0f, hours = 0.0f;
        int   peak = -1;
        if (power_stats_get(g_battery_pct, &avg_ma, &hours, &peak)) {
            char v[32];
            snprintf(v, sizeof(v), "%.1f mA", (double)avg_ma);
            sw_info_add(list, "Average Drain", v, SW_COLOR_TEXT_GREY, SW_FONT_S);
            if (avg_ma > 0.1f && g_battery_pct >= 0) {
                /* 400 mAh cell → 4 mAh per %. */
                float left_mah = (float)g_battery_pct * 4.0f;
                float life_h = left_mah / avg_ma;
                snprintf(v, sizeof(v), "%dh %02dm", (int)life_h,
                         (int)((life_h - (float)(int)life_h) * 60.0f));
                sw_info_add(list, "Est. Battery Life", v,
                            SW_COLOR_TEXT_GREY, SW_FONT_S);
            }
        } else {
            sw_info_add(list, "Average Drain", "measuring…",
                        SW_COLOR_TEXT_GREY, SW_FONT_S);
        }
    }

    sw_list_add_group_gap(list);

    /* ── 4. 80% Charge Limit + Optimized Charging ────────────────── */
    sw_toggle_add(list, "80% Charge Limit",
                  g_config.charge_limit_80, _charge_limit_switch_cb, NULL);
    sw_comment_add(list, "Stop charging when battery reaches 80%.\n"
                        "Helps prolong battery lifespan.");

    if (!g_config.charge_limit_80) {
        sw_toggle_add(list, "Optimized Charging",
                      g_config.optimized_charging_enabled,
                      _optimized_charge_switch_cb, NULL);
        sw_comment_add(list, "Charges slowly to finish ~1h before your\n"
                            "usual morning use. Max 0.7C (275mA).");
    }

    sw_list_add_group_gap(list);

    /* ── 5. Low Power Mode switch (yellow indicator) ─────────────── */
    sw_toggle_add_ex(list,
        &(sw_toggle_cfg_t){ .label = app_lang_get("power.low_power"),
                            .on_color = SW_COLOR_YELLOW,
                            .checked = g_config.low_power_mode },
        _low_power_switch_cb, NULL);
    sw_comment_add(list, "Disable animations, set 15 FPS target,\n"
                        "dim timeout 5s, brightness 80% of\n"
                        "current. Touch to toggle.");
}

static void _build_screen_time_page(void)
{
    lv_obj_t *list = settings_ui_list();

    /* ── 1. Today's usage: big "xh xm" + "per day" unit. ─────────── */
    uint32_t today = app_usage_total_sec(0);
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

    char tbuf[24];
    _fmt_dur(tbuf, sizeof(tbuf), today);
    s_st_today = lv_label_create(card);
    lv_label_set_text(s_st_today, tbuf);
    lv_obj_set_style_text_font(s_st_today, SW_FONT_L, 0);
    lv_obj_set_style_text_color(s_st_today, SW_COLOR_WHITE, 0);
    lv_obj_set_style_pad_left(s_st_today, EOS_WX(20), 0);
    s_st_today_shown = today;

    lv_obj_t *unit = lv_label_create(card);
    lv_label_set_text(unit, "per day");
    lv_obj_set_style_text_font(unit, SW_FONT_S, 0);
    lv_obj_set_style_text_color(unit, SW_COLOR_TEXT_GREY, 0);
    lv_obj_set_style_pad_left(unit, EOS_WX(8), 0);
    lv_obj_set_style_margin_top(unit, EOS_WY(24), 0);   /* near baseline */

    /* ── 2. Weekly stacked graph (trailing 7 calendar days). ─────── */
    sw_week_day_t days[7];
    const char *letters[7];
    uint32_t avg = 0;
    screen_time_week(days, letters, &avg);
    sw_graph_week_card(list, days, letters, avg);

    sw_list_add_group_gap(list);

    /* ── 3. Weekly total info (like "Last Charged to 100%"). ─────── */
    uint32_t wk = app_usage_week_total_sec();
    char wbuf[24];
    _fmt_dur(wbuf, sizeof(wbuf), wk);
    lv_obj_t *panel = sw_info_add(list, "Screen Time This Week", wbuf,
                                  SW_COLOR_TEXT_GREY, SW_FONT_S);
    if (panel && lv_obj_get_child_count(panel) > 1) {
        s_st_week_val = lv_obj_get_child(panel, 1);
        s_st_week_shown = wk;
    }

    sw_list_add_group_gap(list);

    /* ── 4. Per-app usage today (all installed apps, sorted desc). ─ */
    sw_title_add(list, "Apps");

    int total = app_registry_count();
    if (total > 0) {
        uint32_t *secs  = malloc((size_t)total * sizeof(uint32_t));
        int      *order = malloc((size_t)total * sizeof(int));
        if (secs && order) {
            uint32_t max_sec = 0;
            for (int i = 0; i < total; i++) {
                secs[i]   = app_usage_app_sec(i);
                order[i]  = i;
                if (secs[i] > max_sec) max_sec = secs[i];
            }
            /* Stable insertion sort by today's usage (descending). */
            for (int i = 1; i < total; i++) {
                int       v = order[i];
                uint32_t  s = secs[i];
                int j = i - 1;
                while (j >= 0 && secs[order[j]] < s) {
                    order[j + 1] = order[j];
                    j--;
                }
                order[j + 1] = v;
            }
            for (int i = 0; i < total; i++) {
                const app_entry_t *a = app_registry_get(order[i]);
                if (!a) continue;
                _usage_row(list, a, secs[order[i]], max_sec);
            }
        }
        if (secs)  free(secs);
        if (order) free(order);
    }
}

/* Refresh the live values on the visible page (called from the settings
 * coordinator's slow LVGL timer). */
void settings_power_live_update(page_t p)
{
    if (p == P_POWER) {
        if (!s_power_temp) return;
        if (g_cpu_temperature == s_power_cpu_shown &&
            g_pmic_temperature == s_power_pmic_shown) return;
        s_power_cpu_shown  = g_cpu_temperature;
        s_power_pmic_shown = g_pmic_temperature;
        char temp_desc[160];
        int off = snprintf(temp_desc, sizeof(temp_desc), "%s",
                           app_lang_get("power.cpu_protection_desc"));
        if (g_cpu_temperature >= 0.0f || g_pmic_temperature >= 0.0f) {
            off += snprintf(temp_desc + off, sizeof(temp_desc) - off,
                            "\nCurrent:");
            if (g_cpu_temperature >= 0.0f) {
                off += snprintf(temp_desc + off, sizeof(temp_desc) - off,
                                "  CPU %.1f°C", g_cpu_temperature);
            }
            if (g_pmic_temperature >= 0.0f) {
                off += snprintf(temp_desc + off, sizeof(temp_desc) - off,
                                "  PMIC %.1f°C", g_pmic_temperature);
            }
        }
        lv_label_set_text(s_power_temp, temp_desc);
    } else if (p == P_BATTERY_PAGE) {
        if (g_battery_pct != s_batt_pct_shown) {
            if (s_batt_arc) lv_arc_set_value(s_batt_arc, g_battery_pct);
            if (s_batt_pct) {
                char b[8];
                snprintf(b, sizeof(b), "%d%%", g_battery_pct);
                lv_label_set_text(s_batt_pct, b);
            }
            s_batt_pct_shown = g_battery_pct;
        }
    } else if (p == P_SCREEN_TIME) {
        uint32_t today = app_usage_total_sec(0);
        if (s_st_today && today != s_st_today_shown) {
            char b[24];
            _fmt_dur(b, sizeof(b), today);
            lv_label_set_text(s_st_today, b);
            s_st_today_shown = today;
        }
        uint32_t wk = app_usage_week_total_sec();
        if (s_st_week_val && wk != s_st_week_shown) {
            char b[24];
            _fmt_dur(b, sizeof(b), wk);
            lv_label_set_text(s_st_week_val, b);
            s_st_week_shown = wk;
        }
    }
}

void settings_power_build(page_t p)
{
    live_reset_all();
    switch (p) {
    case P_POWER:         _build_power_page();      break;
    case P_BATTERY_PAGE:  _build_battery_page();    break;
    case P_SCREEN_TIME:   _build_screen_time_page(); break;
    case P_POWER_CHIPS:   _build_temp_picker(g_config.power_chips_temp_threshold,
                                             _temp_chips_cb,
                                             "AXP2101 PMIC temperature limit.\n"
                                             "When exceeded, charging is disabled\n"
                                             "until it cools down by 2 °C.");
                          break;
    case P_POWER_CPU:     _build_temp_picker(g_config.cpu_temp_threshold,
                                             _temp_cpu_cb,
                                             "ESP32-S3 CPU temperature limit.\n"
                                             "When exceeded, WiFi and Bluetooth\n"
                                             "are disabled until it cools down\n"
                                             "by 2 °C.");
                          break;
    default:              break;
    }
}
