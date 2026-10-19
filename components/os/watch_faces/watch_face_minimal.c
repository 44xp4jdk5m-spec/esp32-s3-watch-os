/**
 * @file watch_face_minimal.c
 * @brief "Minimal" watch face — a clean digital face built with plain LVGL.
 *
 *        This is the reference implementation that demonstrates how easy it
 *        is to add a new face: implement watch_face_t, register it in
 *        watch_face_registry_init(), and the picker picks it up automatically.
 */
#include "watch_face.h"
#include "ui.h"      /* shared fonts */
#include <stdio.h>

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_time   = NULL;
static lv_obj_t *s_date   = NULL;
static lv_obj_t *s_batt   = NULL;

static lv_obj_t *minimal_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);

    /* Big centered time */
    s_time = lv_label_create(s_screen);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -26);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_text_color(s_time, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_time, &ui_font_NotoSansCondensedBold80, 0);
    lv_obj_set_style_text_letter_space(s_time, 4, 0);

    /* Date under the time */
    s_date = lv_label_create(s_screen);
    lv_obj_align(s_date, LV_ALIGN_CENTER, 0, 62);
    lv_label_set_text(s_date, "");
    lv_obj_set_style_text_color(s_date, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(s_date, &ui_font_NotoSansExtraCondensedSemiBold25, 0);

    /* Battery percent top-right */
    s_batt = lv_label_create(s_screen);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -14, 10);
    lv_label_set_text(s_batt, "--%");
    lv_obj_set_style_text_color(s_batt, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_batt, &ui_font_NotoSansExtraCondensedSemiBold25, 0);

    return s_screen;
}

static void minimal_destroy(void)
{
    if (s_screen) lv_obj_delete(s_screen);
    s_screen = NULL;
    s_time   = NULL;
    s_date   = NULL;
    s_batt   = NULL;
}

static void minimal_update_time(const char *time_str, const char *date_str)
{
    if (s_time && lv_obj_is_valid(s_time)) lv_label_set_text(s_time, time_str);
    if (s_date && lv_obj_is_valid(s_date)) lv_label_set_text(s_date, date_str);
}

static void minimal_update_battery(int pct, bool charging, bool vbus_present)
{
    if (s_batt && lv_obj_is_valid(s_batt)) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(s_batt, buf);
    }
}

const watch_face_t watch_face_minimal = {
    .name           = "Minimal",
    .id             = WATCH_FACE_MINIMAL,
    .create         = minimal_create,
    .destroy        = minimal_destroy,
    .update_time    = minimal_update_time,
    .update_battery = minimal_update_battery,
};
