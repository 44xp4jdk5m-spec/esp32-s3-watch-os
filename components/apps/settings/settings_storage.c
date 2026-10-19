/**
 * @file settings_storage.c
 * @brief Settings > Storage pages (SD card, Google Drive, USB MSC,
 *        WiFi file server).  Built on the sw_list widgets.
 */

#include "settings_ui_internal.h"
#include "sw_list.h"
#include "sw_row.h"
#include "sw_title.h"
#include "sw_comment.h"
#include "sw_icon_btn.h"
#include "sw_toggle.h"
#include "sw_picker.h"
#include "sw_info.h"
#include "sw_icons.h"
#include "app_config.h"
#include "app_lang.h"
#include "storage_manager.h"
#include "sd_cache_manager.h"
#include "tinyusb_msc.h"
#include "wifi_file_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "SETTINGS";

#define GDRIVE_ENABLED()  g_config.storage_gdrive_enabled

/* ── Google Drive: enable/disable switch ───────────────────────────── */
static void _gdrive_enable_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.storage_gdrive_enabled = on;
    app_config_save();
    ESP_LOGI(TAG, "Google Drive auto-upload: %s", on ? "ON" : "OFF");
    settings_ui_refresh();
}

/* ── Google Drive: format picker callback (BMP / JPEG) ─────────────── */
static const bool   _gdrive_format_opts[] = {true, false};
static const char  *_gdrive_format_labels[] = {"BMP (lossless)", "JPEG (smaller)"};
#define GDRIVE_FORMAT_COUNT 2

static void _gdrive_format_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= GDRIVE_FORMAT_COUNT) return;

    g_config.storage_gdrive_upload_bmp = _gdrive_format_opts[idx];
    app_config_save();
    sw_picker_set_selected(lv_obj_get_parent(lv_event_get_target(e)), idx);
    ESP_LOGI(TAG, "Upload format: %s", _gdrive_format_labels[idx]);
}

/* ── Google Drive: folder ID input ─────────────────────────────────── */
static void _gdrive_folder_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "GDrive folder ID: '%s'",
             g_config.storage_gdrive_folder_id);
    if (g_config.storage_gdrive_folder_id[0]) {
        g_config.storage_gdrive_folder_id[0] = '\0';
        app_config_save();
        settings_ui_refresh();
        ESP_LOGI(TAG, "GDrive folder cleared (root)");
    }
}

/* ── Google Drive: token input ─────────────────────────────────────── */
static void _gdrive_token_cb(lv_event_t *e)
{
    (void)e;
    bool has_token = (g_config.storage_gdrive_token[0] != '\0');
    ESP_LOGI(TAG, "GDrive token: %s (%d chars)",
             has_token ? "SET" : "NOT SET",
             (int)strlen(g_config.storage_gdrive_token));
    if (has_token) {
        g_config.storage_gdrive_token[0] = '\0';
        app_config_save();
        settings_ui_refresh();
        ESP_LOGI(TAG, "GDrive token cleared");
    }
}

/* ── USB Mass Storage switch ───────────────────────────────────────── */
static void _usb_msc_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.usb_msc_enabled = on;
    app_config_save();
    if (on) tinyusb_msc_init();
    else    tinyusb_msc_deinit();
    ESP_LOGI(TAG, "USB Mass Storage: %s", on ? "ON" : "OFF");
}

/* ── WiFi File Server switch ───────────────────────────────────────── */
static void _wifi_fs_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.wifi_fs_enabled = on;
    app_config_save();
    if (on) wifi_file_server_start();
    else    wifi_file_server_stop();
    ESP_LOGI(TAG, "WiFi File Server: %s", on ? "ON" : "OFF");
}

/* ====================================================================
 * Page builders
 * ==================================================================== */

static void _build_storage_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, "Manage screenshots & cloud upload");
    sw_list_add_group_gap(list);

    sw_icon_btn_add(list, SW_COLOR_GREY, LV_SYMBOL_SETTINGS, "SD Card",
                    settings_ui_go_nav_cb, (void *)(uintptr_t)P_STORAGE_SD);

    sw_icon_btn_add(list, SW_COLOR_BLUE, LV_SYMBOL_SETTINGS, "Google Drive",
                    settings_ui_go_nav_cb, (void *)(uintptr_t)P_STORAGE_GDRIVE);
    sw_comment_add(list, GDRIVE_ENABLED()
        ? "Auto-upload is ON" : "Auto-upload is OFF");

    sw_list_add_group_gap(list);

    /* Pending uploads status (read-only, auto-height info row). */
    {
        char pending_str[16];
        snprintf(pending_str, sizeof(pending_str), "%" PRIu32,
                 storage_manager_pending_count());
        sw_info_add(list, app_lang_get("storage.pending_uploads"),
                    pending_str, SW_COLOR_TEXT_GREY, SW_FONT_M);
    }

    sw_comment_add(list, "Screenshots are saved when GPIO10+GPIO0\n"
                        "are pressed together.  Upload requires\n"
                        "WiFi and a valid Google Drive token.");
}

static void _build_storage_sd_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, app_lang_get("storage.sd_settings"));
    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("storage.card_info"));

    /* Capacity / status display (auto-height info row). */
    {
        bool sd_ok = sd_cache_is_mounted();
        sw_info_add(list, app_lang_get("storage.status"),
                    sd_ok ? app_lang_get("storage.mounted")
                          : app_lang_get("storage.not_found"),
                    sd_ok ? SW_COLOR_GREEN : SW_COLOR_RED, SW_FONT_S);
    }

    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("storage.tinyusb_msc"));

    /* USB Mass Storage switch (TinyUSB MSC). */
    sw_toggle_add(list, app_lang_get("storage.usb_msc"),
                  g_config.usb_msc_enabled, _usb_msc_switch_cb, NULL);
    sw_comment_add(list, "When ON, connect USB to PC to access\n"
                        "SD card as a USB drive (GPIO19/20).");

    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("storage.wifi_title"));

    /* WiFi File Server switch. */
    sw_toggle_add(list, app_lang_get("storage.wifi_fs"),
                  g_config.wifi_fs_enabled, _wifi_fs_switch_cb, NULL);
    sw_comment_add(list, "Browse SD card via browser at\n"
                        "http://<watch-ip>:8080");
}

static void _build_gdrive_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_comment_add(list, app_lang_get("storage.gdrive_settings"));
    sw_list_add_group_gap(list);
    sw_title_add(list, app_lang_get("storage.auto_upload"));

    /* Enable/disable switch. */
    sw_toggle_add(list, app_lang_get("storage.upload_to_gdrive"),
                  GDRIVE_ENABLED(), _gdrive_enable_switch_cb, NULL);
    sw_comment_add(list, "When ON, screenshots are uploaded\n"
                        "automatically via WiFi.");

    /* Account & Format options — only when enabled. */
    if (GDRIVE_ENABLED()) {
        sw_list_add_group_gap(list);
        sw_title_add(list, app_lang_get("storage.account"));

        /* Folder ID text input. */
        {
            lv_obj_t *row = sw_row_create(list);
            lv_obj_set_style_pad_left(row, EOS_WX(16), 0);

            lv_obj_t *lbl = lv_label_create(row);
            lv_obj_set_style_text_font(lbl, SW_FONT_M, 0);
            lv_label_set_text(lbl, app_lang_get("storage.folder_id"));
            lv_obj_set_flex_grow(lbl, 1);
            lv_obj_set_style_text_color(lbl, SW_COLOR_WHITE, 0);

            char folder_display[48];
            if (g_config.storage_gdrive_folder_id[0]) {
                snprintf(folder_display, sizeof(folder_display), "%.24s...",
                         g_config.storage_gdrive_folder_id);
            } else {
                strcpy(folder_display, "root");
            }
            lv_obj_t *val = lv_label_create(row);
            lv_obj_set_style_text_font(val, SW_FONT_S, 0);
            lv_label_set_text(val, folder_display);
            lv_obj_set_style_text_color(val, SW_COLOR_TEXT_GREY, 0);
            lv_obj_set_style_pad_right(val, EOS_WX(16), 0);
            lv_obj_add_event_cb(row, _gdrive_folder_cb, LV_EVENT_CLICKED, NULL);
            sw_comment_add(list, "Tap to enter a Google Drive folder ID.\n"
                                "Leave empty for root folder.");
        }

        /* Token status. */
        {
            bool has_token = (g_config.storage_gdrive_token[0] != '\0');
            lv_obj_t *row = sw_row_create(list);
            lv_obj_set_style_pad_left(row, EOS_WX(16), 0);

            lv_obj_t *lbl = lv_label_create(row);
            lv_obj_set_style_text_font(lbl, SW_FONT_M, 0);
            lv_obj_set_flex_grow(lbl, 1);
            lv_obj_set_style_text_color(lbl, SW_COLOR_WHITE, 0);
            lv_label_set_text(lbl, app_lang_get("storage.access_token"));

            lv_obj_t *status_lbl = lv_label_create(row);
            lv_obj_set_style_text_font(status_lbl, SW_FONT_S, 0);
            lv_obj_set_style_text_color(status_lbl, has_token
                                        ? SW_COLOR_GREEN : SW_COLOR_RED, 0);
            lv_obj_set_style_pad_right(status_lbl, EOS_WX(16), 0);
            lv_label_set_text(status_lbl, has_token ? app_lang_get("storage.set")
                                                    : app_lang_get("storage.not_set"));
            lv_obj_add_event_cb(row, _gdrive_token_cb, LV_EVENT_CLICKED, NULL);
            sw_comment_add(list, "Tap to paste a new OAuth token.\n"
                                "Obtain token from Google Cloud Console.");
        }

        sw_list_add_group_gap(list);

        /* Format selection — radio picker. */
        sw_title_add(list, "Format");
        sw_comment_add(list, "Choose upload image format");

        int cur_fmt = g_config.storage_gdrive_upload_bmp ? 0 : 1;
        sw_picker_opt_t fmt_opts[GDRIVE_FORMAT_COUNT];
        for (int i = 0; i < GDRIVE_FORMAT_COUNT; i++) {
            fmt_opts[i].label = _gdrive_format_labels[i];
            fmt_opts[i].icon  = NULL;
        }
        lv_obj_t *picker = sw_picker_create(list, fmt_opts, GDRIVE_FORMAT_COUNT,
                                            cur_fmt, false, _gdrive_format_cb);
        (void)picker;
    }
}

void settings_storage_build(page_t p)
{
    switch (p) {
    case P_STORAGE:        _build_storage_page();    break;
    case P_STORAGE_SD:     _build_storage_sd_page(); break;
    case P_STORAGE_GDRIVE: _build_gdrive_page();     break;
    default:               break;
    }
}
