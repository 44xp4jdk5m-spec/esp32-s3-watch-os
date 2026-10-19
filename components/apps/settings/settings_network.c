/**
 * @file settings_network.c
 * @brief Settings > WiFi & Bluetooth pages.
 *
 * WiFi is dynamic: it kicks off a scan on entry, lists APs, connects on
 * tap (keyboard for the password), and refreshes in place while the async
 * scan/connect runs.  The coordinator calls settings_network_enter() /
 * settings_network_leave() to manage the refresh timer + WiFi driver.
 */

#include "settings_ui.h"
#include "settings_ui_internal.h"
#include "sw_list.h"
#include "sw_row.h"
#include "sw_title.h"
#include "sw_comment.h"
#include "sw_toggle.h"
#include "sw_icons.h"
#include "app_config.h"
#include "app_events.h"
#include "app_lang.h"
#include "app_wifi.h"
#include "app_ble_ancs.h"
#include "keyboard.h"
#include "spinner.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "SETTINGS";

/* ── WiFi page state ───────────────────────────────────────────────── */
static char           s_pending_ssid[33];
static bool           s_wifi_scan_requested;
static bool           s_wifi_last_scan_done;
static bool           s_wifi_last_conn_done;
static lv_timer_t    *s_wifi_timer;

static void _wifi_deferred_refresh(void *data)
{
    (void)data;
    settings_ui_refresh();
}

static void _wifi_refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!settings_ui_is_active() || settings_ui_current_page() != P_WIFI)
        return;
    bool scan_done = app_wifi_scan_is_done();
    bool conn_done = app_wifi_connect_is_done();
    bool changed = false;
    if (scan_done != s_wifi_last_scan_done) { s_wifi_last_scan_done = scan_done; changed = true; }
    if (conn_done != s_wifi_last_conn_done) { s_wifi_last_conn_done = conn_done; changed = true; }
    if (changed) settings_ui_refresh();
}

/* ── WiFi lifecycle hooks (called by the coordinator) ──────────────── */
void settings_network_enter(void)
{
    if (settings_ui_current_page() != P_WIFI) return;
    s_wifi_scan_requested = false;
    s_wifi_last_scan_done = app_wifi_scan_is_done();
    s_wifi_last_conn_done = app_wifi_connect_is_done();
    if (!s_wifi_timer) {
        s_wifi_timer = lv_timer_create(_wifi_refresh_timer_cb, 500, NULL);
        lv_timer_set_repeat_count(s_wifi_timer, -1);
    }
}

void settings_network_leave(void)
{
    if (s_wifi_timer) { lv_timer_del(s_wifi_timer); s_wifi_timer = NULL; }
    if (settings_ui_current_page() == P_WIFI) {
        /* Free the WiFi driver if not connected, so a scan alone doesn't
         * leave internal RAM/DMA nearly exhausted. */
        app_wifi_idle_deinit();
    }
}

/* ── WiFi callbacks ────────────────────────────────────────────────── */
static void _wifi_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.wifi_enabled = on;
    app_config_save();
    if (!on) {
        app_wifi_set_enabled(false);
    }
    /* Reset scan state so re-enabling triggers a fresh scan. */
    s_wifi_scan_requested = false;
    s_wifi_last_scan_done = true;
    s_wifi_last_conn_done = true;
    ESP_LOGI(TAG, "WiFi: %s (reboot to connect)", on ? "ON" : "OFF");
    lv_async_call(_wifi_deferred_refresh, NULL);
}

static void _wifi_password_cb(const char *text)
{
    settings_ui_set_header_visible(true);
    if (text && text[0]) {
        app_wifi_connect_to(s_pending_ssid, text);
    }
    lv_async_call(_wifi_deferred_refresh, NULL);
}

static void _wifi_ap_click_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    app_wifi_ap_t ap;
    if (!app_wifi_scan_get(idx, &ap)) return;
    app_events_request_vibe_click();

    strncpy(s_pending_ssid, ap.ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';

    if (ap.authmode == WIFI_AUTH_OPEN) {
        app_wifi_connect_to(ap.ssid, "");
        lv_async_call(_wifi_deferred_refresh, NULL);
    } else {
        /* Open the full-screen keyboard for the password. */
        settings_ui_set_header_visible(false);
        keyboard_open(NULL, _wifi_password_cb);
    }
}

static void _wifi_ap_row(int idx, const app_wifi_ap_t *ap)
{
    lv_obj_t *list = settings_ui_list();
    lv_obj_t *btn = sw_row_create(list);
    lv_obj_set_style_pad_left(btn, EOS_WX(16), 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, SW_FONT_M, 0);
    lv_label_set_text(label, ap->ssid);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_color(label, SW_COLOR_WHITE, 0);

    if (ap->authmode != WIFI_AUTH_OPEN) {
        lv_obj_t *lock = lv_label_create(btn);
        lv_label_set_text(lock, SW_ICON_LOCK);
        lv_obj_set_style_text_font(lock, SW_ICON_F, 0);
        lv_obj_set_style_text_color(lock, SW_COLOR_TEXT_GREY, 0);
        lv_obj_set_style_pad_right(lock, EOS_WX(6), 0);
    }

    lv_obj_t *rssi = lv_label_create(btn);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d dBm", ap->rssi);
    lv_label_set_text(rssi, buf);
    lv_obj_set_style_text_font(rssi, SW_FONT_S, 0);
    lv_obj_set_style_text_color(rssi, SW_COLOR_TEXT_GREY, 0);
    lv_obj_set_style_pad_right(rssi, EOS_WX(16), 0);

    lv_obj_add_event_cb(btn, _wifi_ap_click_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)idx);
}

static void _build_wifi_page(void)
{
    lv_obj_t *list = settings_ui_list();

    /* WiFi master switch. */
    sw_toggle_add(list, app_lang_get("wifi.switch_label"),
                  g_config.wifi_enabled, _wifi_switch_cb, NULL);

    if (!g_config.wifi_enabled) {
        sw_comment_add(list, app_lang_get("wifi.off_desc"));
        sw_list_add_group_gap(list);
        sw_comment_add(list, app_lang_get("wifi.disabled_note"));
        return;
    }

    /* Kick off a scan once per page entry. */
    if (!s_wifi_scan_requested) {
        s_wifi_scan_requested = true;
        app_wifi_scan_begin();
    }

    sw_list_add_group_gap(list);

    /* ── Connected section ── */
    sw_title_add(list, "Connected");
    const char *conn = app_wifi_get_connected_ssid();
    if (app_wifi_is_connected() && conn && conn[0]) {
        lv_obj_t *btn = sw_row_create(list);
        lv_obj_set_style_pad_left(btn, EOS_WX(16), 0);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, SW_FONT_M, 0);
        lv_label_set_text(lbl, conn);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_flex_grow(lbl, 1);
        lv_obj_set_style_text_color(lbl, SW_COLOR_WHITE, 0);
        lv_obj_t *tick = lv_label_create(btn);
        lv_label_set_text(tick, SW_ICON_CHECK);
        lv_obj_set_style_text_font(tick, SW_ICON_F, 0);
        lv_obj_set_style_text_color(tick, SW_THEME_PRIMARY, 0);
        lv_obj_set_style_pad_right(tick, EOS_WX(16), 0);
    } else {
        sw_comment_add(list, "Not connected");
    }

    sw_list_add_group_gap(list);

    /* ── Others WiFi section ── */
    sw_title_add(list, "Others WiFi");

    if (app_wifi_is_connecting()) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Connecting to %s…", s_pending_ssid);
        sw_comment_add(list, buf);
    } else if (app_wifi_connect_is_done() && !app_wifi_connect_ok()) {
        sw_comment_add(list, "Connection failed — check the password");
    } else if (app_wifi_connect_is_done() && app_wifi_connect_ok()) {
        sw_comment_add(list, "Connected");
    }

    if (app_wifi_scan_is_done()) {
        int n = app_wifi_scan_get_count();
        if (n == 0) sw_comment_add(list, "No networks found");
        for (int i = 0; i < n; i++) {
            app_wifi_ap_t ap;
            if (!app_wifi_scan_get(i, &ap)) continue;
            if (ap.ssid[0] == '\0') continue;
            _wifi_ap_row(i, &ap);
        }
    } else {
        spinner_create(list, SPINNER_DEFAULT_DIAMETER);
    }

    sw_comment_add(list, "Tap a network to connect. Encrypted\n"
                         "networks ask for the password on the\n"
                         "keyboard screen.");
}

/* ── Bluetooth page ────────────────────────────────────────────────── */
static void _ble_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.ble_enabled = on;
    g_toggle_ble = true;
    app_config_save();
    ESP_LOGI(TAG, "BLE switch: %s", on ? "ON" : "OFF");
    settings_ui_refresh();
}

static void _ancs_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    g_config.ancs_enabled = on;
    g_toggle_ancs = true;
    ESP_LOGI(TAG, "ANCS switch: %s", on ? "ON" : "OFF");
}

static void _build_bt_page(void)
{
    lv_obj_t *list = settings_ui_list();

    sw_toggle_add(list, app_lang_get("bluetooth.switch_label"),
                  g_config.ble_enabled, _ble_switch_cb, NULL);

    if (g_config.ble_enabled) {
        /* ── Group: BLE features — ANCS + pairing together. ─────── */
        sw_list_add_group_gap(list);

        sw_toggle_add(list, app_lang_get("bluetooth.ancs_label"),
                      g_config.ancs_enabled, _ancs_switch_cb, NULL);

        /* "Connect to iPhone" button. */
        lv_obj_t *pair_btn = sw_row_create(list);
        lv_obj_set_style_pad_left(pair_btn, EOS_WX(16), 0);
        lv_obj_t *pair_lbl = lv_label_create(pair_btn);
        lv_obj_set_style_text_font(pair_lbl, SW_FONT_M, 0);
        lv_label_set_text(pair_lbl, app_lang_get("bluetooth.connect_iphone"));
        lv_obj_set_flex_grow(pair_lbl, 1);
        lv_obj_set_style_text_color(pair_lbl, SW_COLOR_WHITE, 0);
        lv_obj_t *pair_chevron = lv_label_create(pair_btn);
        lv_label_set_text(pair_chevron, SW_ICON_ARROW_FWD);
        lv_obj_set_style_text_font(pair_chevron, SW_ICON_F, 0);
        lv_obj_set_style_text_color(pair_chevron, SW_COLOR_GREY, 0);
        lv_obj_set_style_pad_right(pair_btn, EOS_WX(16), 0);

        /* Connection status. */
        sw_comment_add(list, g_ancs_connected
            ? app_lang_get("bluetooth.connected")
            : g_ancs_passkey[0]
                ? app_lang_get("bluetooth.pairing")
                : app_lang_get("bluetooth.not_connected"));

        /* 6-digit passcode display in round panels. */
        if (g_ancs_passkey[0]) {
            sw_comment_add(list, "Enter this code on your iPhone:");

            lv_obj_t *code_row = lv_obj_create(list);
            lv_obj_set_size(code_row, lv_pct(100), EOS_WY(70));
            lv_obj_set_style_bg_opa(code_row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(code_row, 0, 0);
            lv_obj_set_style_pad_all(code_row, 0, 0);
            lv_obj_set_flex_flow(code_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(code_row, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_add_flag(code_row, LV_OBJ_FLAG_USER_1);

            for (int i = 0; i < 6 && g_ancs_passkey[i]; i++) {
                lv_obj_t *panel = lv_obj_create(code_row);
                lv_obj_set_size(panel, EOS_WX(50), EOS_WY(50));
                lv_obj_set_style_radius(panel, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(panel, SW_THEME_SECONDARY, 0);
                lv_obj_set_style_border_width(panel, 2, 0);
                lv_obj_set_style_border_color(panel, SW_COLOR_BLUE, 0);
                lv_obj_set_style_pad_all(panel, 0, 0);
                lv_obj_set_style_margin_left(panel, EOS_WX(5), 0);
                lv_obj_set_style_margin_right(panel, EOS_WX(5), 0);
                lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(panel, LV_OBJ_FLAG_USER_1);

                char digit[2] = {g_ancs_passkey[i], '\0'};
                lv_obj_t *dl = lv_label_create(panel);
                lv_obj_set_style_text_font(dl, SW_FONT_M, 0);
                lv_obj_set_style_text_color(dl, SW_COLOR_WHITE, 0);
                lv_label_set_text(dl, digit);
                lv_obj_center(dl);
            }
        }
    }
}

void settings_network_build(page_t p)
{
    switch (p) {
    case P_WIFI: _build_wifi_page(); break;
    case P_BT:   _build_bt_page();   break;
    default:     break;
    }
}
