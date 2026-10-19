/**
 * @file upload_notice_ui.c
 * @brief Temporary on-screen notice for upload success/failure.
 *
 * Creates a small rounded banner at screen bottom with an icon + message.
 * Auto-dismisses via LVGL timer after 3 seconds.
 */

#include "upload_notice_ui.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "UPLOAD_NOTICE";

/* Theme colours matching settings_ui */
#define NOTICE_BG_SUCCESS  lv_color_hex(0x04DE71)  /* green */
#define NOTICE_BG_FAILED   lv_color_hex(0xFF3B30)  /* red */
#define NOTICE_BG_CACHED   lv_color_hex(0xFF9500)  /* orange */
#define NOTICE_TEXT_COLOR  lv_color_hex(0xFFFFFF)

static lv_obj_t *s_notice = NULL;
static lv_timer_t *s_dismiss_timer = NULL;

/* ── Dismiss callback ────────────────────────────────────────────────── */

static void _dismiss_cb(lv_timer_t *t)
{
    (void)t;
    upload_notice_hide();
}

/* ── Show ────────────────────────────────────────────────────────────── */

void upload_notice_show(upload_notice_type_t type, const char *message)
{
    /* Remove existing notice and timer */
    upload_notice_hide();

    /* Pick default messages */
    const char *default_msg;
    lv_color_t bg_color;

    switch (type) {
    case UPLOAD_NOTICE_SUCCESS:
        default_msg = "Screenshot uploaded";
        bg_color = NOTICE_BG_SUCCESS;
        break;
    case UPLOAD_NOTICE_FAILED:
        default_msg = "Upload failed";
        bg_color = NOTICE_BG_FAILED;
        break;
    case UPLOAD_NOTICE_CACHED:
        default_msg = "Saved offline (SD)";
        bg_color = NOTICE_BG_CACHED;
        break;
    default:
        default_msg = "";
        bg_color = NOTICE_BG_SUCCESS;
        break;
    }

    if (!message) message = default_msg;

    /* Create notice on the active screen */
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;

    s_notice = lv_obj_create(scr);
    lv_obj_set_size(s_notice, lv_pct(80), 44);
    lv_obj_set_style_bg_color(s_notice, bg_color, 0);
    lv_obj_set_style_bg_opa(s_notice, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_notice, 22, 0);  /* fully rounded */
    lv_obj_set_style_border_width(s_notice, 0, 0);
    lv_obj_set_style_shadow_width(s_notice, 8, 0);
    lv_obj_set_style_shadow_opa(s_notice, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_notice, 0, 0);
    lv_obj_remove_flag(s_notice, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_notice, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* Label */
    lv_obj_t *label = lv_label_create(s_notice);
    lv_label_set_text(label, message);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, NOTICE_TEXT_COLOR, 0);
    lv_obj_center(label);

    /* Auto-dismiss after 3 seconds */
    s_dismiss_timer = lv_timer_create(_dismiss_cb, 3000, NULL);
    lv_timer_set_repeat_count(s_dismiss_timer, 1);

    ESP_LOGI(TAG, "Notice: %s", message);
}

/* ── Hide ────────────────────────────────────────────────────────────── */

void upload_notice_hide(void)
{
    if (s_dismiss_timer) {
        lv_timer_del(s_dismiss_timer);
        s_dismiss_timer = NULL;
    }
    if (s_notice) {
        lv_obj_del(s_notice);
        s_notice = NULL;
    }
}
