/**
 * @file watch_face_classic.c
 * @brief "Classic" watch face — wraps the SquareLine Studio ui_watch_face
 *        screen so it fits the new watch-face interface.
 */
#include "watch_face.h"
#include "ui.h"    /* ui_watch_face, ui_time, ui_date, ui_battery, ui_Image1 */

static lv_obj_t *classic_create(void)
{
    ui_watch_face_screen_init();
    return ui_watch_face;
}

static void classic_destroy(void)
{
    ui_watch_face_screen_destroy();
}

static void classic_update_time(const char *time_str, const char *date_str)
{
    if (ui_time && lv_obj_is_valid(ui_time)) lv_label_set_text(ui_time, time_str);
    if (ui_date && lv_obj_is_valid(ui_date)) lv_label_set_text(ui_date, date_str);
}

static void classic_update_battery(int pct, bool charging, bool vbus_present)
{
    if (ui_battery && lv_obj_is_valid(ui_battery)) {
        lv_arc_set_value(ui_battery, pct);
        lv_color_t color = (pct < 20) ? lv_color_hex(0xFF0000)
                                      : lv_color_hex(0x39FF00);
        lv_obj_set_style_arc_color(ui_battery, color,
                                   LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }
    /* Charge icon on the face — only when USB VBUS is present */
    if (ui_Image1 && lv_obj_is_valid(ui_Image1)) {
        if (vbus_present)
            lv_obj_remove_flag(ui_Image1, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(ui_Image1, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── AOD burn-in prevention: nudge the static elements by (dx,dy) ∈ {0,1}
 * every AOD pixel-shift interval so no OLED subpixel stays lit forever. ── */
static int  s_shift_dx = 0, s_shift_dy = 0;
static bool s_base_saved = false;
static int  s_time_x, s_time_y, s_date_x, s_date_y;
static int  s_batt_x, s_batt_y, s_img1_x, s_img1_y, s_earth_x, s_earth_y;

static void classic_aod_pixel_shift(int dx, int dy)
{
    if (!ui_watch_face || !lv_obj_is_valid(ui_watch_face)) {
        s_base_saved = false;   /* face re-created — re-capture positions */
        return;
    }

    /* Capture the designer-set positions once so we can offset them
     * without accumulating drift (get_x/y respect each object's align). */
    if (!s_base_saved) {
        if (ui_time    && lv_obj_is_valid(ui_time))    { s_time_x = lv_obj_get_x(ui_time);    s_time_y = lv_obj_get_y(ui_time); }
        if (ui_date    && lv_obj_is_valid(ui_date))    { s_date_x = lv_obj_get_x(ui_date);    s_date_y = lv_obj_get_y(ui_date); }
        if (ui_battery && lv_obj_is_valid(ui_battery)) { s_batt_x = lv_obj_get_x(ui_battery); s_batt_y = lv_obj_get_y(ui_battery); }
        if (ui_Image1  && lv_obj_is_valid(ui_Image1))  { s_img1_x = lv_obj_get_x(ui_Image1);  s_img1_y = lv_obj_get_y(ui_Image1); }
        if (ui_earth   && lv_obj_is_valid(ui_earth))   { s_earth_x = lv_obj_get_x(ui_earth);  s_earth_y = lv_obj_get_y(ui_earth); }
        s_base_saved = true;
    }

    if (dx == s_shift_dx && dy == s_shift_dy) return;
    s_shift_dx = dx; s_shift_dy = dy;

    if (ui_time    && lv_obj_is_valid(ui_time))    lv_obj_set_pos(ui_time,    s_time_x + dx,    s_time_y + dy);
    if (ui_date    && lv_obj_is_valid(ui_date))    lv_obj_set_pos(ui_date,    s_date_x + dx,    s_date_y + dy);
    if (ui_battery && lv_obj_is_valid(ui_battery)) lv_obj_set_pos(ui_battery, s_batt_x + dx,    s_batt_y + dy);
    if (ui_Image1  && lv_obj_is_valid(ui_Image1))  lv_obj_set_pos(ui_Image1,  s_img1_x + dx,    s_img1_y + dy);
    if (ui_earth   && lv_obj_is_valid(ui_earth))   lv_obj_set_pos(ui_earth,   s_earth_x + dx,   s_earth_y + dy);
}

const watch_face_t watch_face_classic = {
    .name           = "Classic",
    .id             = WATCH_FACE_CLASSIC,
    .create         = classic_create,
    .destroy        = classic_destroy,
    .update_time    = classic_update_time,
    .update_battery = classic_update_battery,
    .aod_pixel_shift = classic_aod_pixel_shift,
};
