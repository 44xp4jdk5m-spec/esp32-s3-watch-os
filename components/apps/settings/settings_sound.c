/**
 * @file settings_sound.c
 * @brief Settings > Sound & Haptics pages (silent mode, vibrator strength,
 *        pattern picker, duration).  Built on the sw_list widgets.
 */

#include "settings_ui_internal.h"
#include "sw_list.h"
#include "sw_title.h"
#include "sw_comment.h"
#include "sw_entry.h"
#include "sw_toggle.h"
#include "sw_slider.h"
#include "sw_picker.h"
#include "sw_icons.h"
#include "app_config.h"
#include "app_events.h"
#include "app_lang.h"
#include "hw_vibrator.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "SETTINGS";

/* ── Silent Mode switch ────────────────────────────────────────────── */
static void _silent_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (!on) {
        app_events_request_vibe_click();
    }
    g_config.silent_mode = on;
    hw_vibrator_set_muted(on);
    app_config_save();
    ESP_LOGI(TAG, "Silent Mode: %s", on ? "ON" : "OFF");
}

/* ── Haptics: strength slider ──────────────────────────────────────── */
static void _haptic_strength_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *comment = lv_event_get_user_data(e);
    int32_t val = lv_slider_get_value(slider);
    g_config.vibrator_strength = (uint8_t)val;
    app_config_save();
    app_events_request_vibe_click();
    if (comment && lv_obj_is_valid(comment)) {
        lv_label_set_text(comment, val >= 200 ? "High"
                                            : (val >= 120 ? "Medium" : "Low"));
    }
    ESP_LOGI(TAG, "Vibe strength: %d", val);
}

/* ── Haptics: localized pattern name ───────────────────────────────── */
static const char *_vibe_pattern_lang_name(int idx)
{
    switch (idx) {
    case HW_VIBE_PATTERN_CLICK:        return app_lang_get("sound.pattern_normal");
    case HW_VIBE_PATTERN_DOUBLE_CLICK: return app_lang_get("sound.pattern_double");
    case HW_VIBE_PATTERN_TRIPLE_CLICK: return app_lang_get("sound.pattern_short_short");
    case HW_VIBE_PATTERN_HEARTBEAT:    return app_lang_get("sound.pattern_long");
    case HW_VIBE_PATTERN_RISING:       return "Rising";
    case HW_VIBE_PATTERN_DECAYING:     return "Decaying";
    case HW_VIBE_PATTERN_PULSING:      return "Pulsing";
    case HW_VIBE_PATTERN_SIREN:        return "Siren";
    case HW_VIBE_PATTERN_SOS:          return "SOS";
    case HW_VIBE_PATTERN_BUZZ:         return "Buzz";
    default:                           return hw_vibe_pattern_names[idx];
    }
}

/* ── Haptics: pattern picker callback ──────────────────────────────── */
static void _haptic_pattern_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= HW_VIBE_PATTERN_COUNT) return;

    g_config.vibrator_pattern = (uint8_t)idx;
    app_config_save();
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
    ESP_LOGI(TAG, "Vibe pattern: %s", hw_vibe_pattern_names[idx]);

    /* Play the selected pattern immediately (non-blocking task). */
    hw_vibrator_play_pattern((hw_vibe_pattern_t)idx,
                             g_config.vibrator_strength,
                             g_config.vibrator_duration_ms);
}

/* ── Haptics: duration slider callback ─────────────────────────────── */
static void _haptic_duration_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *comment = lv_event_get_user_data(e);
    int32_t val = lv_slider_get_value(slider);
    g_config.vibrator_duration_ms = (uint16_t)val;
    app_config_save();
    app_events_request_vibe_click();
    if (comment && lv_obj_is_valid(comment)) {
        char buf[32];
        if (val >= 1000)
            snprintf(buf, sizeof(buf), "%.1f sec", val / 1000.0f);
        else
            snprintf(buf, sizeof(buf), "%d ms", val);
        lv_label_set_text(comment, buf);
    }
    ESP_LOGI(TAG, "Vibe duration: %dms", val);

    hw_vibrator_play_pattern((hw_vibe_pattern_t)g_config.vibrator_pattern,
                             g_config.vibrator_strength,
                             (uint16_t)val);
}

/* ====================================================================
 * Page builders
 * ==================================================================== */

static void _build_sound_page(void)
{
    lv_obj_t *list = settings_ui_list();

    /* ── Group: Sound ───────────────────────────────────────────── */
    sw_title_add(list, app_lang_get("sound.title"));
    sw_toggle_add(list, app_lang_get("sound.silent_mode"),
                  g_config.silent_mode, _silent_switch_cb, NULL);

    sw_list_add_group_gap(list);

    /* ── Group: Haptics ─────────────────────────────────────────── */
    sw_entry_add(list, app_lang_get("sound.haptics"),
                 settings_ui_go_nav_cb, (void *)(uintptr_t)P_HAPTICS);
    sw_comment_add(list, app_lang_get("sound.haptics_desc"));
}

static void _build_haptics_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, app_lang_get("sound.haptics"));

    /* ── Group: Strength ─────────────────────────────────────────── */
    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("sound.vibrator_strength"));

    sw_slider_t *str_sl = sw_slider_add(list,
                            app_lang_get("sound.vibrator_strength"),
                            SW_ICON_REMOVE, SW_ICON_ADD);
    if (str_sl) {
        lv_slider_set_range(str_sl->slider, 50, HW_VIBRATOR_MAX_DUTY);
        lv_slider_set_value(str_sl->slider, g_config.vibrator_strength,
                            LV_ANIM_OFF);
    }
    lv_obj_t *str_comment = sw_comment_add(list,
        g_config.vibrator_strength >= 200
            ? "High" : (g_config.vibrator_strength >= 120 ? "Medium" : "Low"));
    if (str_sl) {
        lv_obj_add_event_cb(str_sl->slider, _haptic_strength_cb,
                            LV_EVENT_VALUE_CHANGED, str_comment);
    }

    /* ── Group: Pattern ─────────────────────────────────────────── */
    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("sound.vibrator_pattern"));

    static const char *pat_icons[HW_VIBE_PATTERN_COUNT] = {
        [HW_VIBE_PATTERN_CLICK]        = SW_ICON_VOLUME_UP,
        [HW_VIBE_PATTERN_DOUBLE_CLICK] = SW_ICON_VOLUME_DOWN,
        [HW_VIBE_PATTERN_TRIPLE_CLICK] = SW_ICON_VOLUME_DOWN,
        [HW_VIBE_PATTERN_HEARTBEAT]    = SW_ICON_NOTIF_ACTIVE,
        [HW_VIBE_PATTERN_RISING]       = SW_ICON_ARROW_UP,
        [HW_VIBE_PATTERN_DECAYING]     = SW_ICON_ARROW_DOWN,
        [HW_VIBE_PATTERN_PULSING]      = SW_ICON_PAUSE,
        [HW_VIBE_PATTERN_SIREN]        = SW_ICON_EMERGENCY,
        [HW_VIBE_PATTERN_SOS]          = SW_ICON_EMERGENCY,
        [HW_VIBE_PATTERN_BUZZ]         = SW_ICON_VOLUME_UP,
    };

    sw_picker_opt_t opts[HW_VIBE_PATTERN_COUNT];
    int selected = (int)g_config.vibrator_pattern;
    for (int pi = 0; pi < HW_VIBE_PATTERN_COUNT; pi++) {
        opts[pi].label = _vibe_pattern_lang_name(pi);
        opts[pi].icon  = pat_icons[pi];
    }
    lv_obj_t *picker = sw_picker_create(list, opts, HW_VIBE_PATTERN_COUNT,
                                        selected, false, _haptic_pattern_cb);
    (void)picker;

    /* ── Group: Duration ─────────────────────────────────────────── */
    sw_list_add_group_gap(list);
    sw_title_add(list, "Duration");

    sw_slider_t *dur_sl = sw_slider_add(list, "Duration",
                                        SW_ICON_REMOVE, SW_ICON_ADD);
    if (dur_sl) {
        lv_slider_set_range(dur_sl->slider, 50, HW_VIBRATOR_MAX_DURATION_MS);
        lv_slider_set_value(dur_sl->slider, g_config.vibrator_duration_ms,
                            LV_ANIM_OFF);
    }
    {
        char dur_label[32];
        uint16_t d = g_config.vibrator_duration_ms;
        if (d >= 1000)
            snprintf(dur_label, sizeof(dur_label), "%.1f sec", d / 1000.0f);
        else
            snprintf(dur_label, sizeof(dur_label), "%d ms", d);
        lv_obj_t *dur_comment = sw_comment_add(list, dur_label);
        if (dur_sl) {
            lv_obj_add_event_cb(dur_sl->slider, _haptic_duration_cb,
                                LV_EVENT_VALUE_CHANGED, dur_comment);
        }
    }

    sw_list_add_group_gap(list);
    sw_comment_add(list, "Pattern + duration will preview when changed.\n"
                         "Changes apply to all vibration feedback.");
}

void settings_sound_build(page_t p)
{
    switch (p) {
    case P_SOUND:   _build_sound_page();   break;
    case P_HAPTICS: _build_haptics_page(); break;
    default:        break;
    }
}
