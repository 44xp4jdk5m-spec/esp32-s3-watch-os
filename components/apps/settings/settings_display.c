/**
 * @file settings_display.c
 * @brief Settings > Display & Dim pages (brightness, AOD, wake-on-raise,
 *        light sleep, dim timeout).  Built on the sw_list widgets.
 */

#include "settings_ui_internal.h"
#include "sw_list.h"
#include "sw_comment.h"
#include "sw_slider.h"
#include "sw_entry.h"
#include "sw_toggle.h"
#include "sw_learn_more.h"
#include "sw_picker.h"
#include "sw_icons.h"
#include "app_config.h"
#include "app_events.h"
#include "app_power.h"
#include "app_lang.h"
#include "actionsheet.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SETTINGS";

/* ── Brightness slider ─────────────────────────────────────────────── */
static void _brightness_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    uint8_t target = (uint8_t)val;
    g_config.brightness = target;
    app_config_save();
    /* Smooth via for-loop.  Runs in LVGL context so blocking is fine. */
    app_power_set_brightness_smooth(target, 6);
    ESP_LOGI(TAG, "Brightness: %d%%", target);
}

/* ── AOD switch ────────────────────────────────────────────────────── */
static void _aod_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.always_on_display = on;
    app_power_set_aod_enabled(on);
    app_config_save();
    ESP_LOGI(TAG, "AOD switch: %s", on ? "ON" : "OFF");
    /* Refresh in place to show/hide the Dim Time entry. */
    settings_ui_refresh();
}

/* ── Wake on Raise switch ──────────────────────────────────────────── */
static void _wake_raise_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.wake_on_raise_enabled = on;
    app_config_save();
    ESP_LOGI(TAG, "Wake-on-Raise switch: %s", on ? "ON" : "OFF");
}

/* ── Light sleep switch ────────────────────────────────────────────── */
static void _light_sleep_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.light_sleep_enabled = on;
    app_config_save();
    ESP_LOGI(TAG, "Light sleep switch: %s", on ? "ON" : "OFF");
}

static void _light_sleep_learn_more_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    const char *acts[] = { app_lang_get("common.ok") };
    action_sheet_open(SW_ICON_INFO,
                      app_lang_get("display.light_sleep_learn_more"),
                      acts, 1, NULL, NULL);
}

/* ── Dim timeout picker ────────────────────────────────────────────── */
static const uint16_t _dim_opts[] = {5, 15, 30, 60, 300};
#define _DIM_OPT_COUNT  (sizeof(_dim_opts) / sizeof(_dim_opts[0]))

static void _dim_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)_DIM_OPT_COUNT) return;

    g_config.dim_timeout_sec = _dim_opts[idx];
    app_config_save();
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
}

/* ====================================================================
 * Page builders
 * ==================================================================== */

static void _build_display_page(void)
{
    lv_obj_t *list = settings_ui_list();

    /* ── Group: Brightness (most used — top) ────────────────────── */
    sw_slider_t *ls = sw_slider_add(list,
                     app_lang_get("display.brightness"),
                     SW_ICON_REMOVE, SW_ICON_ADD);
    if (ls) {
        lv_slider_set_range(ls->slider, 10, 100);
        lv_slider_set_value(ls->slider, g_config.brightness, LV_ANIM_OFF);
        lv_obj_add_event_cb(ls->slider, _brightness_slider_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }

    sw_list_add_group_gap(list);

    /* ── Group: Dimming — dim timeout + AOD together. ───────────── */
    if (!g_config.always_on_display) {
        sw_entry_add(list, app_lang_get("display.dim_timeout"),
                     settings_ui_go_nav_cb, (void *)(uintptr_t)P_DIM);
        sw_comment_add(list, app_lang_get("display.dim_timeout_desc"));
    }
    lv_obj_t *sw_aod = sw_toggle_add(list,
                     app_lang_get("display.always_on"),
                     g_config.always_on_display, _aod_switch_cb, NULL);
    (void)sw_aod;
    sw_comment_add(list, app_lang_get("display.always_on_desc"));

    sw_list_add_group_gap(list);

    /* ── Group: Wake & Sleep ────────────────────────────────────── */
    /* Wake on Raise (ESP-DL IMU classifier). */
    sw_toggle_add(list, app_lang_get("display.wake_on_raise"),
                  g_config.wake_on_raise_enabled,
                  _wake_raise_switch_cb, NULL);
    sw_comment_add(list, app_lang_get("display.wake_on_raise_desc"));

    /* Light sleep (battery saver while display is off). */
    sw_toggle_add(list, app_lang_get("display.light_sleep"),
                  g_config.light_sleep_enabled,
                  _light_sleep_switch_cb, NULL);
    sw_comment_add(list, app_lang_get("display.light_sleep_desc"));
    sw_learn_more_add(list, _light_sleep_learn_more_cb);
}

static void _build_dim_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Display dims to half brightness after this time");

    uint16_t cur_dim = g_config.dim_timeout_sec;
    static const char *dim_labels[] = {"5 sec", "15 sec", "30 sec", "1 min", "5 min"};

    sw_picker_opt_t opts[_DIM_OPT_COUNT];
    int selected = 0;
    for (int i = 0; i < (int)_DIM_OPT_COUNT; i++) {
        opts[i].label = dim_labels[i];
        opts[i].icon  = NULL;
        if (_dim_opts[i] == cur_dim) selected = i;
    }
    lv_obj_t *picker = sw_picker_create(list, opts, (int)_DIM_OPT_COUNT,
                                        selected, false, _dim_cb);
    (void)picker;
}

void settings_display_build(page_t p)
{
    switch (p) {
    case P_DISP: _build_display_page(); break;
    case P_DIM:  _build_dim_page();     break;
    default:     break;
    }
}
