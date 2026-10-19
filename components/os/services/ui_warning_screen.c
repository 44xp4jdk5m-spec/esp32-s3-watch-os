/**
 * @file ui_warning_screen.c
 * @brief Programmatic thermal warning screen.
 *
 * Full-screen LVGL overlay shown when CPU or PMIC over-temperature is
 * detected.  Blocks the entire UI underneath until the temperature
 * returns to normal.
 */

#include "ui_warning_screen.h"
#include "app_lang.h"
#include "fonts.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "UI_WARN";

#define SCREEN_W  410
#define SCREEN_H  502

/* ── Global state ──────────────────────────────────────────────────── */

static lv_obj_t *s_scr    = NULL;
static lv_obj_t *s_prev   = NULL;  /* screen to restore on hide */

/* ── Helpers ───────────────────────────────────────────────────────── */

static lv_color_t RED    = {0};
static lv_color_t BLACK  = {0};
static lv_color_t WHITE  = {0};
static lv_color_t DGREY  = {0};
static bool       s_colors_inited = false;

static void _init_colors(void)
{
    if (s_colors_inited) return;
    RED   = lv_color_hex(0xFF3B30);
    BLACK = lv_color_hex(0x000000);
    WHITE = lv_color_hex(0xFFFFFF);
    DGREY = lv_color_hex(0x242424);
    s_colors_inited = true;
}

/* ====================================================================
 * PUBLIC API
 * ==================================================================== */

void ui_warning_screen_show(float cpu_temp, float pmic_temp)
{
    if (s_scr && lv_obj_is_valid(s_scr)) {
        ESP_LOGW(TAG, "Warning screen already visible");
        return;
    }

    _init_colors();
    ESP_LOGI(TAG, "Showing warning screen (CPU=%.1f PMIC=%.1f)", cpu_temp, pmic_temp);

    /* Save current screen to restore on hide */
    s_prev = lv_screen_active();

    /* Create new screen */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_scr, BLACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Outer red circle (200×200) ───────────────────────────────── */
    lv_obj_t *outer = lv_obj_create(s_scr);
    lv_obj_set_size(outer, 200, 200);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(outer, BLACK, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(outer, lv_color_hex(0xF62929), 0);
    lv_obj_set_style_border_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(outer, 2, 0);
    lv_obj_set_style_shadow_color(outer, lv_color_hex(0xFF3131), 0);
    lv_obj_set_style_shadow_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(outer, 40, 0);
    lv_obj_set_style_shadow_spread(outer, 10, 0);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(outer, LV_ALIGN_CENTER, 0, -62);

    /* ── Inner red glow dot ───────────────────────────────────────── */
    lv_obj_t *glow = lv_obj_create(outer);
    lv_obj_set_size(glow, 26, 26);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glow, BLACK, 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(glow, lv_color_hex(0xCC0A16), 0);
    lv_obj_set_style_shadow_opa(glow, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(glow, 80, 0);
    lv_obj_set_style_shadow_spread(glow, 50, 0);
    lv_obj_remove_flag(glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(glow);

    /* ── Thermometer stem (vertical bar) ──────────────────────────── */
    lv_obj_t *stem = lv_obj_create(outer);
    lv_obj_set_size(stem, 29, 128);
    lv_obj_set_style_radius(stem, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(stem, BLACK, 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(stem, WHITE, 0);
    lv_obj_set_style_border_opa(stem, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stem, 5, 0);
    lv_obj_remove_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, -10);

    /* ── Thermometer bulb (bottom circle) ─────────────────────────── */
    lv_obj_t *bulb = lv_obj_create(outer);
    lv_obj_set_size(bulb, 50, 50);
    lv_obj_set_style_radius(bulb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bulb, BLACK, 0);
    lv_obj_set_style_bg_opa(bulb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bulb, WHITE, 0);
    lv_obj_set_style_border_opa(bulb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bulb, 5, 0);
    lv_obj_remove_flag(bulb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bulb, LV_ALIGN_CENTER, 0, 52);

    /* ── Red fill inside bulb ─────────────────────────────────────── */
    lv_obj_t *red_bulb = lv_obj_create(outer);
    lv_obj_set_size(red_bulb, 41, 41);
    lv_obj_set_style_radius(red_bulb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(red_bulb, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(red_bulb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(red_bulb, BLACK, 0);
    lv_obj_set_style_border_opa(red_bulb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(red_bulb, 5, 0);
    lv_obj_remove_flag(red_bulb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(red_bulb, LV_ALIGN_CENTER, 0, 52);

    /* ── Small red dot above bulb ─────────────────────────────────── */
    lv_obj_t *red_dot = lv_obj_create(outer);
    lv_obj_set_size(red_dot, 11, 11);
    lv_obj_set_style_bg_color(red_dot, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(red_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(red_dot, 0, 0);
    lv_obj_remove_flag(red_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(red_dot, LV_ALIGN_CENTER, 0, 36);

    /* ── Progress bar (rotated) ───────────────────────────────────── */
    lv_obj_t *bar = lv_slider_create(outer);
    lv_slider_set_value(bar, 50, LV_ANIM_OFF);
    lv_obj_set_size(bar, 128, 19);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, BLACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(bar, 2700, 0);  /* 270° rotation */
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF0000), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(bar, BLACK, LV_PART_INDICATOR);
    lv_obj_set_style_border_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_opa(bar, 0, LV_PART_KNOB);
    lv_obj_align(bar, LV_ALIGN_CENTER, 55, 67);
    /* Compensate LVGL9 draw crash */
    if (lv_obj_get_style_pad_top(bar, LV_PART_MAIN) > 0) {
        lv_obj_set_style_pad_right(bar,
            lv_obj_get_style_pad_right(bar, LV_PART_MAIN) + 1, LV_PART_MAIN);
    }

    /* ── "High Temperature" title ─────────────────────────────────── */
    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, app_lang_get("warning.title"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_text_font(title, &noto_font_24, 0);
    lv_obj_set_width(title, LV_SIZE_CONTENT);
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    lv_obj_align(title, LV_ALIGN_CENTER, -3, 93);

    /* ── "Smart Watch needs to cool down" ─────────────────────────── */
    lv_obj_t *desc = lv_label_create(s_scr);
    lv_label_set_text(desc, app_lang_get("warning.desc"));
    lv_obj_set_style_text_color(desc, WHITE, 0);
    lv_obj_set_style_text_font(desc, &noto_font_24, 0);
    lv_obj_set_width(desc, 275);
    lv_obj_set_height(desc, LV_SIZE_CONTENT);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, 156);

    /* ── Temperature readings (top of screen) ────────────────────── */
    char tmp_buf[128];
    int off = 0;
    if (cpu_temp >= 0.0f) {
        off += snprintf(tmp_buf + off, sizeof(tmp_buf) - off,
                        "CPU: %.1f  C", cpu_temp);
    }
    if (pmic_temp >= 0.0f) {
        if (off > 0) off += snprintf(tmp_buf + off, sizeof(tmp_buf) - off, "    ");
        off += snprintf(tmp_buf + off, sizeof(tmp_buf) - off,
                        "PMIC: %.1f  C", pmic_temp);
    }

    if (off > 0) {
        lv_obj_t *temp_lbl = lv_label_create(s_scr);
        lv_label_set_text(temp_lbl, tmp_buf);
        lv_obj_set_style_text_color(temp_lbl, lv_color_hex(0xFF9500), 0);
        lv_obj_set_style_text_font(temp_lbl, &noto_font_24, 0);
        lv_obj_set_width(temp_lbl, LV_SIZE_CONTENT);
        lv_obj_set_height(temp_lbl, LV_SIZE_CONTENT);
        lv_obj_align(temp_lbl, LV_ALIGN_TOP_MID, 0, 20);
    }

    /* ── "WiFi, Bluetooth & Charging disabled" note ───────────────── */
    lv_obj_t *note = lv_label_create(s_scr);
    lv_label_set_text(note, app_lang_get("warning.disabled"));
    lv_obj_set_style_text_color(note, lv_color_hex(0x9BA0AA), 0);
    lv_obj_set_style_text_font(note, &noto_font_14, 0);
    lv_obj_set_width(note, LV_SIZE_CONTENT);
    lv_obj_set_height(note, LV_SIZE_CONTENT);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* ── Load with fade ───────────────────────────────────────────── */
    lv_screen_load_anim(s_scr, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
}

void ui_warning_screen_hide(void)
{
    if (!s_scr || !lv_obj_is_valid(s_scr)) {
        ESP_LOGW(TAG, "Warning screen not visible");
        return;
    }

    ESP_LOGI(TAG, "Hiding warning screen");

    /* Restore previous screen with fade */
    if (s_prev && lv_obj_is_valid(s_prev)) {
        lv_screen_load_anim(s_prev, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, true);
    }

    s_scr  = NULL;
    s_prev = NULL;
}

bool ui_warning_screen_is_active(void)
{
    return (s_scr != NULL && lv_obj_is_valid(s_scr));
}
