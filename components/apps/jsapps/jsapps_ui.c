/*
 * Watch OS — JavaScript app launcher implementation (see header).
 */
#include "jsapps_ui.h"

#include "js_app_registry.h"
#include "jsapp_run.h"
#include "fonts.h"

#include "esp_log.h"

static const char *TAG = "JSAPPS";

#define LCD_W  410
#define LCD_H  502
#define ROW_H  64

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_prev_screen = NULL;
static bool      s_app_running = false;
static bool      s_direct = false;   /* launched straight from honeycomb */

/* ── launch an app from the grid ──────────────────────────────────── */

/* Completion callback (runs inside the LVGL task).  On success the app
 * screen is already active, so the launcher grid can be deleted.  On
 * failure the runner restored the launcher grid, so we just keep it. */
static void jsapps_launch_done(jsapp_load_result_t result, void *user_data)
{
    (void)user_data;
    if (result == JSAPP_LOAD_OK) {
        if (s_scr) {
            lv_obj_del(s_scr);
            s_scr = NULL;
        }
        s_app_running = true;
    }
}

static void jsapps_launch(int index)
{
    /* Start loading asynchronously — the loading screen (black + icon +
     * dots) is shown immediately.  The launcher grid stays alive as the
     * fallback until jsapps_launch_done fires. */
    if (!jsapp_run_start(index, jsapps_launch_done, NULL))
        ESP_LOGW(TAG, "launch app %d rejected", index);
}

static void row_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "launch app %d", idx);
    jsapps_launch(idx);
}

/* ── grid construction ────────────────────────────────────────────── */
static void grid_build(void)
{
    lv_obj_t *scr = s_scr;

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Apps");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &noto_font_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    int n = js_app_registry_count();

    if (n == 0) {
        lv_obj_t *hint = lv_label_create(scr);
        lv_label_set_text(hint, "No apps installed.\n\nCopy apps to\n/sdcard/watchos/usr/app/<name>/\n(manifest.json + main.js\n+ app_icon.bin)");
        lv_obj_set_style_text_color(hint, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(hint, &noto_font_14, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_size(list, LCD_W, LCD_H - 80);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < n; i++) {
        const js_app_entry_t *e = js_app_registry_get(i);
        if (!e) continue;

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, lv_pct(100), ROW_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1E1E1E), 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        if (e->icon) {
            lv_obj_t *img = lv_image_create(row);
            lv_image_set_src(img, e->icon);
            lv_obj_set_size(img, 48, 48);
            lv_obj_align(img, LV_ALIGN_LEFT_MID, 12, 0);
        }

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, e->name ? e->name : "App");
        lv_obj_set_style_text_color(name, lv_color_white(), 0);
        lv_obj_set_style_text_font(name, &noto_font_24, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 72, 0);
    }
}

/* ── public API ───────────────────────────────────────────────────── */
void jsapps_ui_create(void)
{
    if (s_scr) jsapps_ui_destroy();

    s_prev_screen = lv_screen_active();
    js_app_registry_scan();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_radius(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    grid_build();
    lv_screen_load(s_scr);
    s_app_running = false;
    s_direct = false;
    ESP_LOGI(TAG, "launcher shown (%d apps)", js_app_registry_count());
}

/* Completion callback for a direct (honeycomb) launch. */
static void jsapps_launch_direct_done(jsapp_load_result_t result, void *user_data)
{
    (void)user_data;
    if (result == JSAPP_LOAD_OK) {
        s_app_running = true;        /* honeycomb stays as the exit target */
    } else {
        s_direct = false;            /* runner restored the honeycomb */
        s_prev_screen = NULL;
    }
}

void jsapps_ui_launch_direct(int index)
{
    /* No launcher grid — the app replaces the current screen. */
    s_prev_screen = lv_screen_active();
    s_direct = true;
    s_scr = NULL;
    if (!jsapp_run_start(index, jsapps_launch_direct_done, NULL)) {
        s_direct = false;
        s_prev_screen = NULL;
    }
}

void jsapps_ui_destroy(void)
{
    if (s_app_running || jsapp_run_is_loading()) {
        /* Load the target screen FIRST so the app/loading screen is no
         * longer the ACTIVE screen, then destroy.  Deleting the active
         * screen leaves lv_screen_active() dangling → lv_obj_update_layout()
         * loops forever via lv_obj_get_screen() → task watchdog. */
        if (s_prev_screen && lv_obj_is_valid(s_prev_screen)) {
            lv_screen_load(s_prev_screen);
        }
        jsapp_run_destroy();          /* cancels loading or tears down the app */
        s_app_running = false;
    }
    s_direct = false;

    if (s_prev_screen && lv_obj_is_valid(s_prev_screen)) {
        lv_screen_load_anim(s_prev_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, true);
    } else if (s_scr) {
        lv_obj_del(s_scr);
    }
    s_scr = NULL;
    s_prev_screen = NULL;
    ESP_LOGI(TAG, "launcher destroyed");
}

bool jsapps_ui_is_active(void)
{
    return s_scr != NULL || s_app_running || jsapp_run_is_loading();
}

void jsapps_ui_handle_back(void)
{
    if (jsapp_run_is_loading()) {
        /* Back during the loading animation → cancel the launch. */
        if (s_direct) {
            /* launched straight from honeycomb → cancel, return to menu */
            jsapps_ui_destroy();    /* cancels loading, restores honeycomb */
        } else {
            /* from the grid → cancel; the runner restores the grid */
            jsapp_run_destroy();
        }
        return;
    }

    if (s_app_running) {
        if (s_direct) {
            /* launched straight from honeycomb → exit back to the menu.
             * jsapps_ui_destroy() loads the menu screen before destroying
             * the app so the app screen is never deleted while active. */
            jsapps_ui_destroy();
            return;
        }

        /* otherwise recreate the launcher grid FIRST (making it the active
         * screen), then destroy the app — deleting the active app screen
         * makes lv_obj_update_layout() spin (task watchdog). */
        s_scr = lv_obj_create(NULL);
        lv_obj_set_size(s_scr, LCD_W, LCD_H);
        lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_scr, 0, 0);
        lv_obj_set_style_radius(s_scr, 0, 0);
        lv_obj_set_style_pad_all(s_scr, 0, 0);
        lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

        grid_build();
        lv_screen_load(s_scr);

        jsapp_run_destroy();
        s_app_running = false;
    } else {
        /* launcher → back to the watch menu */
        jsapps_ui_destroy();
    }
}
