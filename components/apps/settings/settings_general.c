/**
 * @file settings_general.c
 * @brief Settings > General & related pages.
 *
 * Covers: General, Language, Time & Region / Region / NTP, System (boot /
 * OTA), OTA Update, Device Info, JS Engine credits, Experimental, Debug,
 * Gesture Recorder, Accessibility.  Also owns the OTA download + gesture
 * recorder timers via settings_general_enter() / settings_general_leave().
 */

#include "settings_ui.h"
#include "settings_ui_internal.h"
#include "sw_list.h"
#include "sw_row.h"
#include "sw_title.h"
#include "sw_comment.h"
#include "sw_icon_btn.h"
#include "sw_entry.h"
#include "sw_toggle.h"
#include "sw_button.h"
#include "sw_slider.h"
#include "sw_picker.h"
#include "sw_icons.h"
#include "app_config.h"
#include "app_events.h"
#include "app_lang.h"
#include "app_power.h"
#include "app_time_region.h"
#include "honeycomb_menu.h"
#include "gesture_recorder.h"
#include "wrist_flick.h"
#include "air_pointer.h"
#include "ota_manager.h"
#include "keyboard.h"
#include "actionsheet.h"
#include "warning.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "SETTINGS";

/* ====================================================================
 * Debug page callbacks
 * ==================================================================== */
static void _debug_log_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.log_enabled = on;
    app_config_save();
    esp_log_level_set("*", on ? ESP_LOG_VERBOSE : ESP_LOG_NONE);
    ESP_LOGI(TAG, "Logging: %s", on ? "ON" : "OFF");
}

static void _debug_cpu_monitor_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.cpu_monitor_enabled = on;
    app_config_save();
    if (on) lv_sysmon_show_performance(NULL);
    else    lv_sysmon_hide_performance(NULL);
    ESP_LOGI(TAG, "LVGL CPU Monitor: %s", on ? "ON" : "OFF");
}

/* ====================================================================
 * Language picker
 * ==================================================================== */
static const char *_lang_codes[] = {LANG_CODE_EN, LANG_CODE_TC, LANG_CODE_JP};
#define LANG_PICKER_COUNT 3

static void _lang_picker_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= LANG_PICKER_COUNT) return;

    const char *new_lang = _lang_codes[idx];
    if (strcmp(new_lang, g_config.lang_code) == 0) return;

    app_lang_set(new_lang);
    app_config_save();
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
    ESP_LOGI(TAG, "Language: %s", app_lang_name(new_lang));

    /* Keep the header title current (Language page). */
    settings_ui_header_set_title(app_lang_get("language.title"));
    /* Refresh the whole settings UI to update all labels. */
    settings_ui_refresh();
}

/* ====================================================================
 * Region / NTP pickers
 * ==================================================================== */
static void _reg_cb(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    g_config.region_index = idx;
    g_config.timezone_offset = app_time_region_get_tz(idx);
    g_config.ntp_url_index = 0;
    app_config_save();
    /* Go back to the parent (Time & Region). */
    settings_ui_go(settings_ui_parent_page(settings_ui_current_page()));
}

static int s_ntp_total;

static void _ntp_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ntp_total) return;

    g_config.ntp_url_index = (uint8_t)idx;
    app_config_save();
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
}

/* ====================================================================
 * OTA page (Settings > System > OTA)
 * ==================================================================== */
static int        s_ota_last_refresh = -1;
static lv_timer_t *s_ota_timer;

static void _ota_url_cb(const char *text)
{
    settings_ui_set_header_visible(true);
    if (text) {
        strncpy(g_config.ota_url, text, sizeof(g_config.ota_url) - 1);
        g_config.ota_url[sizeof(g_config.ota_url) - 1] = '\0';
        app_config_save();
    }
    settings_ui_refresh();
}

static void _ota_edit_url_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    settings_ui_set_header_visible(false);
    keyboard_open(g_config.ota_url, _ota_url_cb);
}

static void _ota_install_cb(lv_event_t *e)
{
    (void)e;
    ota_state_t st = ota_manager_get_state();
    if (st == OTA_DOWNLOAD_DONE) {
        ota_manager_install();
    } else if (st == OTA_IDLE || st == OTA_DOWNLOAD_FAILED) {
        ota_manager_start_download();
    }
}

static void _ota_refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!settings_ui_is_active() || settings_ui_current_page() != P_OTA)
        return;
    int now = ota_manager_get_progress() * 10 + (int)ota_manager_get_state();
    if (now != s_ota_last_refresh) {
        s_ota_last_refresh = now;
        settings_ui_refresh();
    }
}

static void _build_ota_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Firmware .bin URL. Serve your build\n"
                         "over HTTP and paste the link below.");

    /* ── Firmware URL row ──────────────────────────────────────── */
    lv_obj_t *url_btn = sw_row_create(list);
    lv_obj_set_style_pad_left(url_btn, EOS_WX(16), 0);
    lv_obj_t *url_lbl = lv_label_create(url_btn);
    lv_obj_set_style_text_font(url_lbl, SW_FONT_M, 0);
    lv_label_set_text(url_lbl, "Firmware URL");
    lv_obj_set_flex_grow(url_lbl, 1);
    lv_obj_set_style_text_color(url_lbl, SW_COLOR_WHITE, 0);
    lv_obj_t *url_val = lv_label_create(url_btn);
    lv_obj_set_style_text_font(url_val, SW_FONT_M, 0);
    lv_label_set_text(url_val, g_config.ota_url[0] ? g_config.ota_url : "Not set");
    lv_label_set_long_mode(url_val, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(url_val, SW_COLOR_GREY, 0);
    lv_obj_set_style_pad_right(url_btn, EOS_WX(16), 0);
    lv_obj_add_event_cb(url_btn, _ota_edit_url_cb, LV_EVENT_CLICKED, NULL);

    sw_list_add_group_gap(list);

    /* ── Status ────────────────────────────────────────────────── */
    ota_state_t st = ota_manager_get_state();
    int prog = ota_manager_get_progress();
    char buf[96];
    switch (st) {
    case OTA_IDLE:            snprintf(buf, sizeof buf, "Idle — set a URL, then press Install"); break;
    case OTA_DOWNLOADING:     snprintf(buf, sizeof buf, "Downloading… %d%%", prog); break;
    case OTA_DOWNLOAD_DONE:   snprintf(buf, sizeof buf, "Download complete — press Install"); break;
    case OTA_DOWNLOAD_FAILED: snprintf(buf, sizeof buf, "Download failed — check the URL"); break;
    case OTA_INSTALLING:      snprintf(buf, sizeof buf, "Installing…"); break;
    default:                  snprintf(buf, sizeof buf, ""); break;
    }
    sw_comment_add(list, buf);

    sw_list_add_group_gap(list);

    /* ── Uniform full-width Install button (spinner while downloading) ── */
    bool downloading = (st == OTA_DOWNLOADING);

    lv_obj_t *btn = sw_button_add(list,
                                  downloading ? "Downloading..." : "Install",
                                  SW_COLOR_WHITE,
                                  downloading ? NULL : _ota_install_cb);
    if (downloading) {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        /* Spinner next to the label (the button is flex-centred). */
        lv_obj_t *sp = lv_spinner_create(btn);
        lv_obj_set_size(sp, EOS_WX(22), EOS_WY(22));
        lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_color(sp, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_color(sp, lv_color_hex(0x404040), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_margin_left(sp, EOS_WX(8), 0);
    }

    sw_comment_add(list, "Downloads the .bin, then installs it\nand reboots.");
}

/* ── Boot / OTA switches ───────────────────────────────────────────── */
static void _boot_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.skip_boot_screen = on;
    app_config_save();
    ESP_LOGI(TAG, "Skip boot screen: %s", on ? "ON" : "OFF");
}

static void _ota_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.ota_enabled = on;
    app_config_save();
    ESP_LOGI(TAG, "OTA switch: %s", on ? "ON" : "OFF");
    settings_ui_refresh();
}

/* ====================================================================
 * Gesture Recorder (temporary) — collect IMU data for the wrist-flick
 * back-gesture model.  Settings > General > Experimental.
 * ==================================================================== */
static char        s_gesture_name[32] = GESTURE_REC_DEFAULT_NAME;
static lv_obj_t   *s_gesture_status   = NULL;
static lv_obj_t   *s_gesture_rec_btn  = NULL;
static lv_obj_t   *s_gesture_mark_btn = NULL;
static lv_timer_t *s_gesture_timer    = NULL;

static void _gesture_sanitize_name(void)
{
    char tmp[32] = {0};
    int  j = 0;
    for (int i = 0; s_gesture_name[i] && j < (int)sizeof(tmp) - 5; i++) {
        char c = s_gesture_name[i];
        if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == ',') continue;
        tmp[j++] = c;
    }
    if (!strstr(tmp, ".csv")) strncat(tmp, ".csv", sizeof(tmp) - strlen(tmp) - 1);
    strncpy(s_gesture_name, tmp, sizeof(s_gesture_name) - 1);
    s_gesture_name[sizeof(s_gesture_name) - 1] = '\0';
}

static void _gesture_name_done(const char *text)
{
    if (text && *text) {
        strncpy(s_gesture_name, text, sizeof(s_gesture_name) - 1);
        s_gesture_name[sizeof(s_gesture_name) - 1] = '\0';
        _gesture_sanitize_name();
    }
    settings_ui_refresh();
}

static void _gesture_edit_name_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    keyboard_open(s_gesture_name, _gesture_name_done);
}

static void _gesture_record_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    if (gesture_recorder_is_recording()) {
        gesture_recorder_stop();
    } else {
        gesture_recorder_clear();
        gesture_recorder_start();
    }
}

static void _gesture_set_mode(gesture_rec_mode_t mode, const char *def_name)
{
    if (gesture_recorder_get_mode() == mode) return;
    gesture_recorder_set_mode(mode);
    strncpy(s_gesture_name, def_name, sizeof(s_gesture_name) - 1);
    s_gesture_name[sizeof(s_gesture_name) - 1] = '\0';
    settings_ui_refresh();
}

static void _gesture_mode_pick_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    settings_ui_vibe();
    _gesture_set_mode(idx == 0 ? GESTURE_REC_MODE_FLICK : GESTURE_REC_MODE_NOTFLICK,
                      idx == 0 ? GESTURE_REC_FLICK_NAME : GESTURE_REC_NOTFLICK_NAME);
}

static void _gesture_mark_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    if (s_gesture_status && lv_obj_is_valid(s_gesture_status)) {
        char buf[96];
        if (!gesture_recorder_is_recording()) {
            lv_label_set_text(s_gesture_status, "Start recording first");
        } else if (gesture_recorder_mark_flick()) {
            snprintf(buf, sizeof(buf), "Flick %lu marked!",
                     (unsigned long)gesture_recorder_flick_count());
            lv_label_set_text(s_gesture_status, buf);
        } else {
            lv_label_set_text(s_gesture_status, "Wait a moment - not enough samples yet");
        }
    }
}

static void _gesture_save_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    if (gesture_recorder_is_recording()) gesture_recorder_stop();
    esp_err_t r = gesture_recorder_save(s_gesture_name);
    if (s_gesture_status && lv_obj_is_valid(s_gesture_status)) {
        char buf[96];
        if (r == ESP_OK) {
            if (gesture_recorder_get_mode() == GESTURE_REC_MODE_FLICK) {
                snprintf(buf, sizeof(buf), "Saved %lu flicks ->\ngestures/%s",
                         (unsigned long)gesture_recorder_flick_count(), s_gesture_name);
            } else {
                snprintf(buf, sizeof(buf), "Saved %lu samples ->\ngestures/%s",
                         (unsigned long)gesture_recorder_sample_count(), s_gesture_name);
            }
            lv_label_set_text(s_gesture_status, buf);
        } else if (r == ESP_ERR_NOT_FOUND) {
            lv_label_set_text(s_gesture_status, "No SD card - mount and retry");
        } else if (r == ESP_ERR_NO_MEM) {
            lv_label_set_text(s_gesture_status,
                gesture_recorder_get_mode() == GESTURE_REC_MODE_FLICK
                    ? "No flicks marked yet - tap Mark"
                    : "Nothing recorded yet");
        } else {
            lv_label_set_text(s_gesture_status, "Save failed - check SD card");
        }
    }
}

static void _gesture_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!settings_ui_is_active() || settings_ui_current_page() != P_GESTURE_REC)
        return;

    bool rec = gesture_recorder_is_recording();
    if (s_gesture_rec_btn && lv_obj_is_valid(s_gesture_rec_btn)) {
        lv_label_set_text(s_gesture_rec_btn, rec ? "Stop recording" : "Start recording");
        lv_obj_set_style_text_color(s_gesture_rec_btn,
            rec ? SW_COLOR_RED : SW_COLOR_GREEN, 0);
    }
    if (s_gesture_mark_btn && lv_obj_is_valid(s_gesture_mark_btn)) {
        bool can_mark = rec && gesture_recorder_get_mode() == GESTURE_REC_MODE_FLICK;
        if (can_mark) {
            lv_obj_clear_state(lv_obj_get_parent(s_gesture_mark_btn), LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(lv_obj_get_parent(s_gesture_mark_btn), LV_STATE_DISABLED);
        }
        char buf[40];
        snprintf(buf, sizeof(buf), "Mark flick (%lu)",
                 (unsigned long)gesture_recorder_flick_count());
        lv_label_set_text(s_gesture_mark_btn, buf);
    }
    if (s_gesture_status && lv_obj_is_valid(s_gesture_status)) {
        uint32_t n = gesture_recorder_sample_count();
        char buf[96];
        if (rec) {
            snprintf(buf, sizeof(buf),
                     "Recording... %lu samples (%.1f s)\nPeak gyro: %.0f deg/s",
                     (unsigned long)n, n / 50.0f, (double)gesture_recorder_peak_gyro());
        } else if (gesture_recorder_get_mode() == GESTURE_REC_MODE_FLICK) {
            snprintf(buf, sizeof(buf), "Ready - %lu flicks marked (%.1f s of motion)",
                     (unsigned long)gesture_recorder_flick_count(), n / 50.0f);
        } else {
            snprintf(buf, sizeof(buf), "Ready - %lu samples captured (%.1f s)",
                     (unsigned long)n, n / 50.0f);
        }
        lv_label_set_text(s_gesture_status, buf);
    }
}

/* ── Wrist flick / air pointer / flick confidence ──────────────────── */
static void _wrist_flick_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.wrist_flick_enabled = on;
    app_config_save();
    if (on) wrist_flick_start();
    ESP_LOGI(TAG, "Wrist flick back gesture: %s", on ? "ON" : "OFF");
}

static void _air_pointer_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.air_pointer_enabled = on;
    app_config_save();
    if (on) air_pointer_start();
    else    air_pointer_stop();
    ESP_LOGI(TAG, "Motion pointer: %s", on ? "ON" : "OFF");
}

static void _flick_conf_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(sl);
    if (v < 20) v = 20;
    if (v > 99) v = 99;
    g_config.wrist_flick_confidence_pct = (uint8_t)v;
    app_config_save();
    ESP_LOGI(TAG, "Flick confidence: %d%%", (int)v);
}

/* ====================================================================
 * Experimental page
 * ==================================================================== */
static void _prescale_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    honeycomb_menu_set_pre_scaled_icons(on);
    ESP_LOGI(TAG, "Pre-scaled icons: %s", on ? "ON (96×96 RGB565)" : "OFF (raw PNG)");
}

static const int _res_opts[] = {1, 2, 3, 4, 5};
#define _RES_OPT_COUNT 5

static void _res_div_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= _RES_OPT_COUNT) return;
    honeycomb_menu_set_res_divisor(_res_opts[idx]);
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
}

/* ====================================================================
 * Page builders
 * ==================================================================== */

static void _build_general_page(void)
{
    lv_obj_t *list = settings_ui_list();
    sw_entry_add(list, app_lang_get("general.language"),
                 settings_ui_go_nav_cb, (void *)(uintptr_t)P_LANG);
    sw_entry_add(list, app_lang_get("general.experimental"),
                 settings_ui_go_nav_cb, (void *)(uintptr_t)P_EXPERIMENTAL);
    sw_entry_add(list, app_lang_get("general.debug"),
                 settings_ui_go_nav_cb, (void *)(uintptr_t)P_DEBUG);
}

static void _build_lang_page(void)
{
    lv_obj_t *list = settings_ui_list();
    sw_comment_add(list, app_lang_get("language.title"));

    int cur_lang_idx = 0;
    for (int i = 0; i < LANG_PICKER_COUNT; i++) {
        if (strcmp(g_config.lang_code, _lang_codes[i]) == 0) { cur_lang_idx = i; break; }
    }

    sw_picker_opt_t lang_opts[LANG_PICKER_COUNT];
    for (int i = 0; i < LANG_PICKER_COUNT; i++) {
        lang_opts[i].label = app_lang_name(_lang_codes[i]);
        lang_opts[i].icon  = NULL;
    }
    lv_obj_t *picker = sw_picker_create(list, lang_opts, LANG_PICKER_COUNT,
                                        cur_lang_idx, true, _lang_picker_cb);
    (void)picker;
}

static void _build_system_page(void)
{
    lv_obj_t *list = settings_ui_list();

    /* Boot behaviour (skip boot-up screen, reboot). */
    sw_icon_btn_add(list, SW_COLOR_TEAL, SW_ICON_POWER, "Boot",
                    settings_ui_go_nav_cb, (void *)(uintptr_t)P_BOOT);
    sw_comment_add(list, "Skip the boot ring, reboot the watch.");

    sw_list_add_group_gap(list);

    /* Firmware updates over WiFi. */
    sw_icon_btn_add(list, SW_COLOR_BLUE, SW_ICON_CLOUD, "Update",
                    settings_ui_go_nav_cb, (void *)(uintptr_t)P_UPDATE);
    sw_comment_add(list, "Flash new firmware over WiFi.");

    sw_list_add_group_gap(list);

    /* Back up / restore the system config. */
    sw_icon_btn_add(list, SW_COLOR_GREY, SW_ICON_SETTINGS, "Configure",
                    settings_ui_go_nav_cb, (void *)(uintptr_t)P_CONFIGURE);
    sw_comment_add(list, "Back up / restore the system config.");

    sw_list_add_group_gap(list);
    sw_entry_add(list, app_lang_get("general.device_info"),
                 settings_ui_go_nav_cb, (void *)(uintptr_t)P_DEVINFO);
    sw_entry_add(list, "JS Engine (QuickJS)",
                 settings_ui_go_nav_cb, (void *)(uintptr_t)P_CREDITS);
}

/* ── Boot page (Settings > System > Boot) ──────────────────────────── */
static void _reboot_confirm_cb(int idx, void *user_data)
{
    (void)user_data;
    if (idx != 1) return;   /* only "Reboot" (0 = Cancel) */
    ESP_LOGI(TAG, "Reboot confirmed — rebooting");
    app_power_reboot();
}

static void _reboot_btn_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    const char *acts[] = { "Cancel", "Reboot" };
    action_sheet_open_ex(SW_ICON_POWER,
                         "Restart the watch now?\nSettings are saved first.",
                         acts, 2, true, _reboot_confirm_cb, NULL);
}

static void _build_boot_page(void)
{
    lv_obj_t *list = settings_ui_list();

    /* Skip boot-up screen. */
    sw_toggle_add(list, "Skip boot-up screen",
                  g_config.skip_boot_screen, _boot_switch_cb, NULL);
    sw_comment_add(list, "Boot straight to the watch face —\nno boot ring shown.");

    sw_list_add_group_gap(list);

    sw_title_add(list, "Restart");
    sw_button_add(list, "Reboot", SW_COLOR_RED, _reboot_btn_cb);
    sw_comment_add(list, "Restart the watch. Unsaved settings\nare saved first.");
}

/* ── Update page (Settings > System > Update) ──────────────────────── */
static void _build_update_page(void)
{
    lv_obj_t *list = settings_ui_list();

    /* OTA over WiFi — folds further options when the switch is off. */
    sw_toggle_add(list, "OTA updates",
                  g_config.ota_enabled, _ota_switch_cb, NULL);
    sw_comment_add(list, "Flash new firmware over WiFi —\nno cable needed.");
    if (g_config.ota_enabled) {
        sw_list_add_group_gap(list);
        sw_entry_add(list, "OTA update",
                     settings_ui_go_nav_cb, (void *)(uintptr_t)P_OTA);
        sw_comment_add(list, "Download the .bin, then press Install.");
    }
}

/* ── Configure page (Settings > System > Configure) ────────────────── */
static void _backup_btn_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    esp_err_t r = app_config_backup_to_sd();
    const char *msg;
    if (r == ESP_OK) {
        msg = "Config backed up to\n/sdcard/watchos/sys/config/.";
    } else if (r == ESP_ERR_NOT_FOUND) {
        msg = "SD card not mounted.\nInsert an SD card and retry.";
    } else {
        msg = "Backup failed — check the SD card.";
    }
    warning_show(msg, NULL, NULL);
}

static void _restore_confirm_cb(int idx, void *user_data)
{
    (void)user_data;
    if (idx != 1) return;   /* only "Restore" (0 = Cancel) */

    esp_err_t r = app_config_restore_from_sd();
    const char *msg;
    if (r == ESP_OK) {
        msg = "Config restored from the SD card.\nReboot to fully apply.";
    } else if (r == ESP_ERR_NOT_FOUND) {
        msg = "No backup found on the SD card.\nBack up first.";
    } else {
        msg = "Restore failed — check the SD card.";
    }
    warning_show(msg, NULL, NULL);
}

static void _restore_btn_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    const char *acts[] = { "Cancel", "Restore" };
    action_sheet_open_ex(SW_ICON_WARNING,
                         "Overwrite the current config with\nthe backup on the SD card?",
                         acts, 2, true, _restore_confirm_cb, NULL);
}

static void _build_configure_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Back up or restore the system\nconfig on the SD card.");
    sw_list_add_group_gap(list);

    sw_title_add(list, "Backup");
    sw_button_add(list, "Backup", SW_COLOR_WHITE, _backup_btn_cb);
    sw_comment_add(list, "Save the current config to\n/sdcard/watchos/sys/config/.");

    sw_list_add_group_gap(list);

    sw_title_add(list, "Restore");
    sw_button_add(list, "Restore", SW_COLOR_RED, _restore_btn_cb);
    sw_comment_add(list, "Overwrite the config with the\nbackup on the SD card.");
}

static void _build_devinfo_page(void)
{
    lv_obj_t *list = settings_ui_list();
    sw_comment_add(list, app_lang_get("device_info.model_desc"));
    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("device_info.version_title"));
    sw_comment_add(list, app_lang_get("device_info.version"));
    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("device_info.model_title"));
    sw_comment_add(list, app_lang_get("device_info.model"));
}

static void _build_credits_page(void)
{
    lv_obj_t *list = settings_ui_list();
    sw_comment_add(list, "JavaScript engine");
    sw_list_add_group_gap(list);

    /* Square QR, same width as the other full-width panels (410 − 2×10
     * list padding = 390).  The QR widget draws its own white background. */
    lv_obj_t *wrap = lv_obj_create(list);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_set_size(wrap, lv_pct(100), EOS_WX(390));
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *qr = lv_qrcode_create(wrap);
    lv_qrcode_set_size(qr, EOS_WX(390));
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_set_style_radius(qr, SW_LIST_RADIUS, 0);
    lv_obj_set_style_clip_corner(qr, true, 0);

    static const char qr_url[] = "https://github.com/bellard/quickjs";
    lv_qrcode_update(qr, qr_url, sizeof(qr_url) - 1);

    sw_list_add_group_gap(list);
    sw_comment_add(list, "QuickJS — a small embeddable JavaScript engine by Fabrice Bellard.\nScan to view the GitHub repository.");
}

static void _build_experimental_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, app_lang_get("experimental.desc"));
    sw_list_add_group_gap(list);

    sw_toggle_add(list, app_lang_get("experimental.prescaled_icons"),
                  honeycomb_menu_get_pre_scaled_icons(),
                  _prescale_switch_cb, NULL);
    sw_comment_add(list, app_lang_get("experimental.prescaled_desc"));

    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("experimental.resolution_scale"));
    sw_comment_add(list, "Downscale internal resolution (RAM-only)");

    static const char *res_labels[] = {"Full", "1/2", "1/3", "1/4", "1/5"};
    int cur = honeycomb_menu_get_res_divisor();
    sw_picker_opt_t res_opts[_RES_OPT_COUNT];
    int sel_res = 0;
    for (int i = 0; i < _RES_OPT_COUNT; i++) {
        res_opts[i].label = res_labels[i];
        res_opts[i].icon  = NULL;
        if (_res_opts[i] == cur) sel_res = i;
    }
    lv_obj_t *picker = sw_picker_create(list, res_opts, _RES_OPT_COUNT,
                                        sel_res, false, _res_div_cb);
    (void)picker;

    sw_list_add_group_gap(list);
    sw_entry_add(list, "Gesture Recorder",
                 settings_ui_go_nav_cb, (void *)(uintptr_t)P_GESTURE_REC);
    sw_comment_add(list, "Record IMU data to the SD card\n"
                         "for training the back gesture.");
}

static void _build_debug_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, app_lang_get("debug.desc"));
    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("debug.system_title"));

    sw_toggle_add(list, app_lang_get("debug.log"),
                  g_config.log_enabled, _debug_log_switch_cb, NULL);
    sw_comment_add(list, app_lang_get("debug.log_desc"));

    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("debug.display_title"));

    sw_toggle_add(list, app_lang_get("debug.cpu_monitor"),
                  g_config.cpu_monitor_enabled,
                  _debug_cpu_monitor_switch_cb, NULL);
    sw_comment_add(list, app_lang_get("debug.cpu_monitor_desc"));
}

static void _build_gesture_rec_page(void)
{
    lv_obj_t *list = settings_ui_list();

    /* Reset rebuild-able widget pointers (the timer must never touch a
     * freed object, e.g. the Mark button is absent in NotFlick mode). */
    s_gesture_rec_btn  = NULL;
    s_gesture_mark_btn = NULL;
    s_gesture_status   = NULL;

    sw_comment_add(list, "Record IMU data for the wrist-flick\n"
                         "back-gesture model, then save to SD.\n"
                         "Files land in /sdcard/gestures/.");
    sw_list_add_group_gap(list);

    /* File name (tap to rename via keyboard). */
    lv_obj_t *name_btn = sw_row_create(list);
    lv_obj_set_style_pad_left(name_btn, EOS_WX(16), 0);
    lv_obj_t *name_lbl = lv_label_create(name_btn);
    lv_obj_set_style_text_font(name_lbl, SW_FONT_M, 0);
    lv_label_set_text(name_lbl, "File name");
    lv_obj_set_flex_grow(name_lbl, 1);
    lv_obj_set_style_text_color(name_lbl, SW_COLOR_WHITE, 0);
    lv_obj_t *name_val = lv_label_create(name_btn);
    lv_obj_set_style_text_font(name_val, SW_FONT_M, 0);
    lv_label_set_text(name_val, s_gesture_name);
    lv_obj_set_style_text_color(name_val, SW_COLOR_GREY, 0);
    lv_obj_set_style_pad_right(name_btn, EOS_WX(16), 0);
    lv_obj_add_event_cb(name_btn, _gesture_edit_name_cb, LV_EVENT_CLICKED, NULL);

    sw_list_add_group_gap(list);

    /* Mode: Flick vs NotFlick (segmented picker). */
    sw_title_add(list, "Mode");
    {
        sw_picker_opt_t mode_opts[2] = {
            { "Flick",    NULL },
            { "NotFlick", NULL },
        };
        int sel_mode = (gesture_recorder_get_mode() == GESTURE_REC_MODE_NOTFLICK) ? 1 : 0;
        lv_obj_t *picker = sw_picker_create_segmented(list, mode_opts, 2,
                                                      sel_mode, _gesture_mode_pick_cb);
        (void)picker;
    }
    sw_comment_add(list,
        gesture_recorder_get_mode() == GESTURE_REC_MODE_FLICK
            ? "Do a flick, then tap Mark below.\nRepeat for every flick."
            : "Record idle / normal movements.\nNo marking needed.");

    sw_list_add_group_gap(list);

    /* Record / Stop. */
    lv_obj_t *rec_btn = sw_row_create(list);
    lv_obj_set_style_pad_left(rec_btn, EOS_WX(16), 0);
    s_gesture_rec_btn = lv_label_create(rec_btn);
    lv_obj_set_style_text_font(s_gesture_rec_btn, SW_FONT_M, 0);
    lv_label_set_text(s_gesture_rec_btn, gesture_recorder_is_recording()
                       ? "Stop recording" : "Start recording");
    lv_obj_set_style_text_color(s_gesture_rec_btn,
        gesture_recorder_is_recording() ? SW_COLOR_RED : SW_COLOR_GREEN, 0);
    lv_obj_set_flex_grow(s_gesture_rec_btn, 1);
    lv_obj_add_event_cb(rec_btn, _gesture_record_cb, LV_EVENT_CLICKED, NULL);

    /* Mark flick (FLICK mode only). */
    if (gesture_recorder_get_mode() == GESTURE_REC_MODE_FLICK) {
        lv_obj_t *mark_btn = sw_row_create(list);
        lv_obj_set_style_pad_left(mark_btn, EOS_WX(16), 0);
        s_gesture_mark_btn = lv_label_create(mark_btn);
        lv_obj_set_style_text_font(s_gesture_mark_btn, SW_FONT_M, 0);
        lv_label_set_text(s_gesture_mark_btn, "Mark flick (0)");
        lv_obj_set_style_text_color(s_gesture_mark_btn, SW_THEME_PRIMARY, 0);
        lv_obj_set_flex_grow(s_gesture_mark_btn, 1);
        lv_obj_add_event_cb(mark_btn, _gesture_mark_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_state(mark_btn, LV_STATE_DISABLED);
    }

    /* Save. */
    lv_obj_t *save_btn = sw_row_create(list);
    lv_obj_set_style_pad_left(save_btn, EOS_WX(16), 0);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_obj_set_style_text_font(save_lbl, SW_FONT_M, 0);
    lv_label_set_text(save_lbl, "Save to SD card");
    lv_obj_set_flex_grow(save_lbl, 1);
    lv_obj_set_style_text_color(save_lbl, SW_COLOR_WHITE, 0);
    lv_obj_add_event_cb(save_btn, _gesture_save_cb, LV_EVENT_CLICKED, NULL);

    sw_list_add_group_gap(list);

    /* Status (refreshed by the timer below). */
    s_gesture_status = sw_comment_add(list, "");
    _gesture_timer_cb(NULL);

    if (!s_gesture_timer) {
        s_gesture_timer = lv_timer_create(_gesture_timer_cb, 200, NULL);
        lv_timer_set_repeat_count(s_gesture_timer, -1);
    }
}

static void _font_scale_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(sl);
    if (v < 0) v = 0;
    if (v > 2) v = 2;
    g_config.font_scale = (uint8_t)v;
    app_config_save();
    ESP_LOGI(TAG, "Font size: %s",
             v == 0 ? "Small" : (v == 1 ? "Normal" : "Big"));
}

/* ── Visibility page (Settings > Accessibility > Visibility) ───────── */
static void _build_visibility_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Readability options.\n"
                         "Small / Normal / Big text.");

    sw_list_add_group_gap(list);

    lv_obj_t *fsl = sw_font_slider_add(list, "Text Size", _font_scale_cb);
    if (fsl) {
        lv_slider_set_value(fsl, g_config.font_scale, LV_ANIM_OFF);
    }
    sw_comment_add(list, "Three steps. The saved size will be\n"
                         "applied to the watch text later.");
}

/* ── Gesture page (Settings > Accessibility > Gesture) ─────────────── */
static void _build_gesture_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Gesture detection runs on-device\n"
                         "with a locally trained model.");
    sw_list_add_group_gap(list);
    sw_title_add(list, "Gestures");

    sw_toggle_add(list, "Wrist Flick Back",
                  g_config.wrist_flick_enabled,
                  _wrist_flick_switch_cb, NULL);
    sw_comment_add(list, "Flick your wrist to go back\n"
                         "while using a sub-screen.");

    sw_toggle_add(list, "Motion Pointer",
                  g_config.air_pointer_enabled,
                  _air_pointer_switch_cb, NULL);
    sw_comment_add(list, "Tilt to move a cursor, double-tap\n"
                         "the watch to click, edge to scroll.");

    sw_list_add_group_gap(list);
    sw_slider_t *ls = sw_slider_add(list, "Flick Confidence",
                                    SW_ICON_REMOVE, SW_ICON_ADD);
    if (ls) {
        lv_slider_set_range(ls->slider, 20, 99);
        lv_slider_set_value(ls->slider, g_config.wrist_flick_confidence_pct,
                            LV_ANIM_OFF);
        lv_obj_add_event_cb(ls->slider, _flick_conf_slider_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }
    sw_comment_add(list, "Higher = stricter (fewer false\n"
                         "triggers). Lower = easier to fire.");
}

/* ── Accessibility index page ──────────────────────────────────────── */
static void _build_accessibility_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Display and gesture assistive\n"
                         "options, all on-device.");

    sw_list_add_group_gap(list);

    sw_icon_btn_add(list, SW_COLOR_YELLOW, SW_ICON_VISIBILITY,
                    "Visibility",
                    settings_ui_go_nav_cb, (void *)(uintptr_t)P_VISIBILITY);
    sw_comment_add(list, "Text size and readability.");

    sw_list_add_group_gap(list);

    sw_icon_btn_add(list, SW_COLOR_PURPLE, SW_ICON_EMERGENCY,
                    "Gesture",
                    settings_ui_go_nav_cb, (void *)(uintptr_t)P_GESTURE);
    sw_comment_add(list, "Wrist flick, motion pointer, flick\n"
                         "confidence.");
}

static void _build_time_region_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Set your timezone and NTP calibration server");
    sw_list_add_group_gap(list);

    const time_region_t *cur_r = app_time_region_get(g_config.region_index);
    char region_info[64];
    if (cur_r) {
        snprintf(region_info, sizeof(region_info), "%s  %s",
                 cur_r->name, cur_r->timezone_label);
    } else {
        snprintf(region_info, sizeof(region_info), "Hong Kong  UTC+8 HKT");
    }
    sw_entry_add(list, region_info,
                 settings_ui_go_nav_cb, (void *)(uintptr_t)P_REGION);

    sw_list_add_group_gap(list);

    const char *cur_ntp = app_time_region_get_ntp_url(
        g_config.region_index, g_config.ntp_url_index);
    char ntp_info[56];
    snprintf(ntp_info, sizeof(ntp_info), "%s", cur_ntp ? cur_ntp : "pool.ntp.org");
    sw_entry_add(list, ntp_info,
                 settings_ui_go_nav_cb, (void *)(uintptr_t)P_NTP);

    sw_list_add_group_gap(list);
    sw_comment_add(list, "NTP is used to calibrate the RTC on boot.\n"
                         "Choose a server close to your region.");
}

static void _build_region_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Choose your region / timezone");

    uint8_t reg_count = app_time_region_get_count();
    uint8_t cur_reg = g_config.region_index;

    for (uint8_t i = 0; i < reg_count; i++) {
        const time_region_t *r = app_time_region_get(i);
        if (!r) continue;

        lv_obj_t *btn = sw_row_create(list);
        lv_obj_set_style_pad_left(btn, EOS_WX(16), 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_obj_set_style_text_font(label, SW_FONT_M, 0);
        lv_label_set_text(label, r->name);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_color(label, SW_COLOR_WHITE, 0);

        lv_obj_t *tz_label = lv_label_create(btn);
        lv_obj_set_style_text_font(tz_label, SW_FONT_S, 0);
        lv_label_set_text(tz_label, r->timezone_label);
        lv_obj_set_style_text_color(tz_label, SW_COLOR_TEXT_GREY, 0);
        lv_obj_set_style_pad_right(tz_label, EOS_WX(8), 0);

        lv_obj_t *tick = lv_label_create(btn);
        lv_label_set_text(tick, SW_ICON_CHECK);
        lv_obj_set_style_text_font(tick, SW_ICON_F, 0);
        lv_obj_set_style_text_color(tick, SW_THEME_PRIMARY, 0);
        lv_obj_set_style_pad_right(tick, EOS_WX(16), 0);
        if (i != cur_reg) lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_event_cb(btn, _reg_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }
}

static void _build_ntp_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Choose NTP calibration server");

    const time_region_t *r = app_time_region_get(g_config.region_index);
    uint8_t ntp_cnt = r ? r->ntp_count : 0;
    uint8_t cur_ntp = g_config.ntp_url_index;

    #define NTP_GLOBAL_COUNT 3
    static const char *global_urls[NTP_GLOBAL_COUNT] = {
        "pool.ntp.org", "time.google.com", "time.cloudflare.com"
    };
    bool show_global = (ntp_cnt > 0);

    int total_opts = (int)ntp_cnt + (show_global ? NTP_GLOBAL_COUNT : 0);
    s_ntp_total = total_opts;

    sw_picker_opt_t opts[total_opts > 0 ? total_opts : 1];
    int selected = 0;
    for (int i = 0; i < total_opts; i++) {
        const char *url;
        if (i < (int)ntp_cnt) {
            url = r->ntp_urls[i];
        } else {
            url = global_urls[i - (int)ntp_cnt];
        }
        opts[i].label = url;
        opts[i].icon  = NULL;
        if (i == (int)cur_ntp) selected = i;
    }
    if (total_opts > 0) {
        lv_obj_t *picker = sw_picker_create(list, opts, total_opts,
                                            selected, false, _ntp_cb);
        (void)picker;
    }
}

/* ====================================================================
 * Page builder + lifecycle
 * ==================================================================== */

void settings_general_build(page_t p)
{
    switch (p) {
    case P_GENERAL:      _build_general_page();     break;
    case P_LANG:         _build_lang_page();        break;
    case P_SYSTEM:       _build_system_page();      break;
    case P_BOOT:         _build_boot_page();        break;
    case P_UPDATE:       _build_update_page();      break;
    case P_CONFIGURE:    _build_configure_page();   break;
    case P_OTA:          _build_ota_page();         break;
    case P_DEVINFO:      _build_devinfo_page();     break;
    case P_CREDITS:      _build_credits_page();     break;
    case P_EXPERIMENTAL: _build_experimental_page(); break;
    case P_DEBUG:        _build_debug_page();       break;
    case P_GESTURE_REC:  _build_gesture_rec_page(); break;
    case P_ACCESSIBILITY:_build_accessibility_page(); break;
    case P_VISIBILITY:   _build_visibility_page();  break;
    case P_GESTURE:      _build_gesture_page();     break;
    case P_TIME_REGION:  _build_time_region_page(); break;
    case P_REGION:       _build_region_page();      break;
    case P_NTP:          _build_ntp_page();         break;
    default:             break;
    }
}

void settings_general_enter(void)
{
    if (settings_ui_current_page() == P_OTA) {
        s_ota_last_refresh = -1;
        if (!s_ota_timer) {
            s_ota_timer = lv_timer_create(_ota_refresh_timer_cb, 500, NULL);
            lv_timer_set_repeat_count(s_ota_timer, -1);
        }
    }
}

void settings_general_leave(void)
{
    /* Stop both timers unconditionally (NULL-safe) — called on page exit
     * and on app destroy. */
    if (s_ota_timer)      { lv_timer_del(s_ota_timer);      s_ota_timer = NULL; }
    if (s_gesture_timer)  { lv_timer_del(s_gesture_timer);  s_gesture_timer = NULL; }
}
