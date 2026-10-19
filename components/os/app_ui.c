/**
 * @file app_ui.c
 * @brief UI management: time, battery, charging screen with arc animation.
 */

#include "app_ui.h"
#include "app_config.h"
#include "app_events.h"
#include "app_power.h"
#include "app_ble_ancs.h"
#include "app_notification_center.h"
#include "app_notification_store.h"
#include "hw_display.h"
#include "hw_rtc.h"
#include "watch_face.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_UI";

static const char *month_names[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *wday_names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

/* Timestamp of last screen change — used to guard against
 * modifying widgets during screen transition animations. */
static int64_t g_screen_changed_us = 0;

/* ── Time display (UTC→local via configurable timezone) ──────────────── */

void app_ui_update_time(void)
{
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return;

    time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
    hw_rtc_time_t local;
    hw_rtc_from_unix(ts, &local);

    int h12 = local.hour % 12;
    if (h12 == 0) h12 = 12;

    char time_buf[16];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", h12, local.min);

    char date_buf[48];
    const char *wday = (local.wday < 7) ? wday_names[local.wday] : "";
    const char *mon  = (local.month >= 1 && local.month <= 12)
                        ? month_names[local.month - 1] : "";
    snprintf(date_buf, sizeof(date_buf), "%s, %d %s", wday, local.day, mon);

    /* Dispatch to the currently active watch face */
    watch_face_update_current_time(time_buf, date_buf);
}

/* ── Battery display ────────────────────────────────────────────────── */

void app_ui_update_battery(void)
{
    /* Skip battery updates during screen transition animations
     * (200ms fade) — modifying widget styles while LVGL is also
     * animating the screen causes invalidation conflicts → WDT. */
    if (esp_timer_get_time() - g_screen_changed_us < 300000) return;

    /* Dispatch to the currently active watch face */
    watch_face_update_current_battery(g_battery_pct, g_is_charging, g_is_vbus_present);
}

/* ── Charging screen (floating overlay) ────────────────────────────── */

static int64_t  g_charge_anim_done_us = 0;
static bool     g_charge_woke_display = false;
static app_screen_t g_prev_screen = APP_SCREEN_WATCH_FACE;

/* Called by LVGL when the arc animation finishes */
static void arc_anim_done_cb(lv_anim_t *a)
{
    g_charge_anim_done_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Arc animation done — holding 1s");
}

static void go_back_to_prev(void)
{
    if (g_current_screen != APP_SCREEN_CHARGING) return;
    g_current_screen = g_prev_screen;
    g_screen_changed_us = esp_timer_get_time();

    /* Hide the floating overlay. The underlying screen was never
     * switched, so nothing needs to be restored. */
    if (ui_charging && lv_obj_is_valid(ui_charging)) {
        lv_anim_del(ui_charging, NULL);   /* cancel any in-flight fade */
        lv_obj_add_flag(ui_charging, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(ui_charging, LV_OPA_COVER, 0);
    }
    if (g_prev_screen == APP_SCREEN_WATCH_FACE) {
        app_ui_update_time();
        /* Don't call app_ui_update_battery() here — the main loop
         * updates it every 500ms. Calling it mid-transition can
         * trigger LVGL invalidation loops → WDT. */
    }
    g_charge_woke_display = false;
}

/* Make sure the charging UI lives on the top layer so it floats over
 * WHATEVER screen LVGL is currently rendering (watch face, menu,
 * settings, photos, ...). Reparenting happens once. */
static void charge_ensure_overlay(void)
{
    if (!ui_charging || !lv_obj_is_valid(ui_charging)) return;

    /* ui_charging is created on the top layer in ui_charging_screen_init().
     * Re-parent only as a robustness fallback (it's a normal top-layer
     * child now, so this CAN succeed — unlike a screen, which LVGL 9.3
     * refuses to reparent). */
    lv_obj_t *top = lv_layer_top();
    if (lv_obj_get_parent(ui_charging) != top) {
        lv_obj_set_parent(ui_charging, top);
    }
    lv_display_t *d = lv_display_get_default();
    lv_obj_set_size(ui_charging,
                    lv_display_get_horizontal_resolution(d),
                    lv_display_get_vertical_resolution(d));
    lv_obj_center(ui_charging);
    /* The whole overlay is read-only — don't intercept touches */
    lv_obj_remove_flag(ui_charging, LV_OBJ_FLAG_CLICKABLE);
    if (ui_Image3 && lv_obj_is_valid(ui_Image3)) {
        lv_obj_remove_flag(ui_Image3, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void show_charge_screen(void)
{
    /* Wake display if dimmed/off (auto-wake, no timer reset) */
    if (g_display_off || g_display_dimmed) {
        app_power_wake_auto();
        g_charge_woke_display = true;
    }

    if (g_current_screen != APP_SCREEN_CHARGING)
        g_prev_screen = g_current_screen;

    g_current_screen = APP_SCREEN_CHARGING;
    g_charge_anim_done_us = 0;
    g_screen_changed_us = esp_timer_get_time();

    /* Floating overlay — works on any screen */
    charge_ensure_overlay();
    if (ui_charging && lv_obj_is_valid(ui_charging)) {
        lv_obj_set_style_opa(ui_charging, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(ui_charging, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui_charging);   /* above the settings header */
        lv_obj_fade_in(ui_charging, 200, 0);
    }
    if (ui_charge) lv_arc_set_value(ui_charge, 0);

    /* Haptic feedback — the charging animation is a system event */
    app_events_request_vibe_click();

    /* Animate arc from 0→battery% with deceleration.
     * Use local lv_anim_t (not static) so re-trigger works cleanly. */
    int target = (g_battery_pct > 0) ? g_battery_pct : 1;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui_charge);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_value);
    lv_anim_set_values(&a, 0, target);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_delay(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, arc_anim_done_cb);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "Charging screen — animating arc 0→%d%%", target);
}

void app_ui_check_charging(void)
{
    /* IMPORTANT: This function MUST be called while holding the LVGL
     * display lock — it drives the charging overlay (top layer) and
     * starts/stops LVGL animations.  main.c calls it early in the
     * loop so the charging animation appears on ANY screen. */

    int64_t now = esp_timer_get_time();

    /* Rule 1: Arc done + 1s hold → dismiss */
    if (g_current_screen == APP_SCREEN_CHARGING &&
        g_charge_anim_done_us != 0 &&
        now - g_charge_anim_done_us > 1000000) {
        go_back_to_prev();
        g_was_vbus_present = g_is_vbus_present;
        g_was_charging     = g_is_charging;
        g_last_charge_change_us = now;
        ESP_LOGI(TAG, "Charging screen dismissed");
        return;
    }

    /* Rule 2: USB unplugged while screen showing → dismiss now */
    if (g_current_screen == APP_SCREEN_CHARGING && !g_is_vbus_present) {
        lv_anim_delete(ui_charge, (lv_anim_exec_xcb_t)lv_arc_set_value);
        go_back_to_prev();
        g_was_vbus_present = false;
        g_was_charging     = false;
        g_last_charge_change_us = now;
        return;
    }

    /* Rule 3: 1s debounce */
    if (now - g_last_charge_change_us < 1000000) return;

    /* Rule 4: USB plugged in (false→true) → show charging screen */
    if (g_is_vbus_present && !g_was_vbus_present) {
        ESP_LOGI(TAG, "USB PLUGGED IN");
        show_charge_screen();
        g_last_charge_change_us = now;
        g_was_vbus_present = true;
        g_was_charging     = true;
        return;
    }

    /* Rule 5: USB unplugged (true→false) → sync state only */
    if (!g_is_vbus_present && g_was_vbus_present) {
        g_was_vbus_present = false;
        g_was_charging     = false;
        g_last_charge_change_us = now;
    }
}

/* ── ANCS Notification display ─────────────────────────────────────── */

void app_ui_check_notifications(void)
{
    if (!g_new_notification_pending) return;
    g_new_notification_pending = false;

    ancs_notification_t notif;
    memcpy(&notif, &g_latest_notification, sizeof(notif));

    ESP_LOGI(TAG, "ANCS: [%s] %s — %s",
             notif.has_app_id ? notif.app_id : "?",
             notif.has_title ? notif.title : "",
             notif.has_message ? notif.message : "");

    /* Store the notification so the notification center can show it.
     * Skip silent notifications (flag bit 0) unless they carry content. */
    if (notif.has_title || notif.has_message) {
        app_notification_store_add_ancs(&notif);
        app_notification_center_refresh();
    }
}
