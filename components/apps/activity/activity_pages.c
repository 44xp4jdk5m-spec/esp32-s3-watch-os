/**
 * @file activity_pages.c
 * @brief Activity app screen builders (ported from my_ui / SquareLine).
 *
 * This is a faithful, for-loop refactored port of the SquareLine-generated
 * ui_activity.c / ui_move.c / ui_exercise.c found in /my_ui:
 *
 *   - The three rings screen (Activity) keeps the corner buttons, live
 *     clock, the three concentric arcs and the ring symbols.
 *   - The three detail pages (Move / Exercise / Stand) share ONE layout
 *     builder parameterised by a theme table.  Only the colours, the
 *     title, the big value + unit, the percentage and the hourly bars
 *     differ between them — everything repetitive (arcs, gridlines, axis
 *     labels, dotted x-axis markings, bars) is generated in for-loops.
 *
 * The Stand page did not exist in my_ui; it follows the same layout with
 * a blue theme, 12 hourly bars and gradient-only bars (no dim bars).
 *
 * The chart is a rolling 24 h window ending at the current time — the four
 * 6-hour section labels are computed from the RTC so the CURRENT section
 * is always the rightmost one and the label order changes over the day.
 * The big value / unit / percentage come from live IMU metrics.
 *
 * Memory model: screens are LVGL objects allocated via the PSRAM-backed
 * LVGL allocator and deleted by the app on destroy().
 */

#include "activity_pages.h"

#include "activity_assets.h"
#include "app_image_loader.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── Runtime-loaded icons (SPIFFS -> PSRAM) ──────────────────────────
 * The old compiled const arrays (ui_img_*.c) are now shipped as .bin
 * under /spiffs/usr/share/activity/ and loaded here — see
 * app_image_loader.h.  They are loaded once (first app open) and kept for
 * the session: they are tiny (~10 KB) and the app's exit uses a 200 ms
 * fade that still renders the page, so freeing in destroy() would be
 * unsafe. */
static bool s_icons_loaded = false;
static lv_image_dsc_t *s_img_weekly_summary = NULL;
static lv_image_dsc_t *s_img_workload       = NULL;
static lv_image_dsc_t *s_img_share          = NULL;
static lv_image_dsc_t *s_img_award          = NULL;
static lv_image_dsc_t *s_img_move           = NULL;
static lv_image_dsc_t *s_img_exercise       = NULL;

void activity_pages_load_icons(void)
{
    if (s_icons_loaded) {
        return;
    }
    s_img_weekly_summary = app_img_load_raw(
        "/spiffs/usr/share/activity/ui_img_weekly_summary_png.bin",
        38, 37, LV_COLOR_FORMAT_I8);
    s_img_workload = app_img_load_raw(
        "/spiffs/usr/share/activity/ui_img_workload_png.bin",
        41, 20, LV_COLOR_FORMAT_I8);
    s_img_share = app_img_load_raw(
        "/spiffs/usr/share/activity/ui_img_share_png.bin",
        44, 29, LV_COLOR_FORMAT_I8);
    s_img_award = app_img_load_raw(
        "/spiffs/usr/share/activity/ui_img_award_png.bin",
        39, 42, LV_COLOR_FORMAT_I8);
    s_img_move = app_img_load_raw(
        "/spiffs/usr/share/activity/ui_img_move_symbol_png.bin",
        13, 16, LV_COLOR_FORMAT_RGB565A8);
    s_img_exercise = app_img_load_raw(
        "/spiffs/usr/share/activity/ui_img_exercise_symbol_png.bin",
        17, 15, LV_COLOR_FORMAT_RGB565A8);
    s_icons_loaded = true;
}

/* ── Arc configuration ─────────────────────────────────────────────── */
typedef struct {
    lv_coord_t size;     /* arc width/height   */
    lv_coord_t width;    /* stroke width       */
    uint32_t   bg_color; /* track colour       */
    uint32_t   ind_color;/* indicator colour   */
    int        value;    /* 0..100             */
} ring_cfg_t;

/* ── Detail-page theme (one entry per page) ────────────────────────── */
typedef struct {
    const char *title;
    uint32_t bg_top;          /* screen gradient top            */
    uint32_t bg_bottom;       /* screen gradient bottom         */
    uint32_t accent;          /* title + big value colour       */
    uint32_t dim;             /* percentage + chart label colour */
    uint32_t bar_grad_top;    /* bar gradient top               */
    uint32_t bar_grad_bottom; /* bar gradient bottom            */
    const char *unit_suffix;  /* "CAL" / "MIN" / "HR"          */
    int   goal;               /* 500 / 30 / 12                  */
    const float *bar_hour;    /* absolute hour-of-day of each bar (0..24) */
    const uint8_t *bar_h;     /* bar heights in %               */
    int   bar_count;
} detail_theme_t;

/* Move hourly bars (from ui_move.c Container11) — hour of day. */
static const float s_move_bar_hour[] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 6.0f,
                                         6.5f, 11.5f, 12.0f, 17.5f, 18.0f, 23.5f };
static const uint8_t s_move_bar_h[]   = { 41, 10, 69, 69, 54, 86, 54, 54, 35, 54, 54, 18 };

/* Exercise hourly bars (from ui_exercise.c Container5) — hour of day. */
static const float s_ex_bar_hour[] = { 0.0f, 0.5f, 1.0f, 17.5f, 18.0f };
static const uint8_t s_ex_bar_h[]  = { 41, 10, 69, 54, 54 };

/* Stand hourly bars — 12 bars across the day, ALL gradient (no dim bars). */
static const float s_stand_bar_hour[] = { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22 };
static const uint8_t s_stand_bar_h[]  = { 58, 42, 66, 50, 30, 22, 18, 25, 20, 28, 22, 15 };

static const detail_theme_t s_themes[3] = {
    /* ── MOVE (red) ─────────────────────────────────────────────── */
    {
        .title = "Move",
        .bg_top = 0x890025, .bg_bottom = 0x28000B,
        .accent = 0xFF0049, .dim = 0xC07F90,
        .bar_grad_top = 0xFF0083, .bar_grad_bottom = 0xF60000,
        .unit_suffix = "CAL", .goal = 500,
        .bar_hour = s_move_bar_hour, .bar_h = s_move_bar_h, .bar_count = 12,
    },
    /* ── EXERCISE (green) ───────────────────────────────────────── */
    {
        .title = "Exercise",
        .bg_top = 0x428200, .bg_bottom = 0x142600,
        .accent = 0x9EFF00, .dim = 0x8EA867,
        .bar_grad_top = 0x9EFF00, .bar_grad_bottom = 0x00E100,
        .unit_suffix = "MIN", .goal = 30,
        .bar_hour = s_ex_bar_hour, .bar_h = s_ex_bar_h, .bar_count = 5,
    },
    /* ── STAND (blue) ───────────────────────────────────────────── */
    {
        .title = "Stand",
        .bg_top = 0x007A93, .bg_bottom = 0x00222A,
        .accent = 0x00BDE6, .dim = 0x6FB2C0,
        .bar_grad_top = 0x8FE8FF, .bar_grad_bottom = 0x00BDE6,
        .unit_suffix = "HR", .goal = 12,
        .bar_hour = s_stand_bar_hour, .bar_h = s_stand_bar_h, .bar_count = 12,
    },
};

/* ══════════════════════════════════════════════════════════════════════
 * Shared helpers
 * ══════════════════════════════════════════════════════════════════════ */

/* Build one concentric arc.  Not clickable — presses pass through to the
 * screen so swipe gestures can start anywhere on the page. */
static lv_obj_t *activity_make_arc(lv_obj_t *parent, const ring_cfg_t *rc)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, rc->size, rc->size);
    lv_obj_set_align(arc, LV_ALIGN_CENTER);
    lv_arc_set_value(arc, rc->value);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Track */
    lv_obj_set_style_arc_color(arc, lv_color_hex(rc->bg_color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, 255, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, rc->width, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    /* Indicator */
    lv_obj_set_style_arc_color(arc, lv_color_hex(rc->ind_color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, 255, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, rc->width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    /* Knob hidden */
    lv_obj_set_style_opa(arc, 0, LV_PART_KNOB);

    return arc;
}

/* ══════════════════════════════════════════════════════════════════════
 * Rings screen (the Activity page)
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *activity_pages_build_rings(activity_page_ui_t *out)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);   /* receive swipe events */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x212221), 0);
    lv_obj_set_style_bg_opa(scr, 255, 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x0A0A09), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

    /* ── Four circular action buttons in the corners ────────────────
     * {x, y, align, image} — all 70x70 with a translucent white disc. */
    struct {
        lv_coord_t x, y;
        lv_align_t align;
        const lv_image_dsc_t *img;
    } s_corners[4] = {
        {  28,  39, LV_ALIGN_TOP_LEFT,     s_img_weekly_summary },
        { -28,  39, LV_ALIGN_TOP_RIGHT,    s_img_workload },
        {  28, -39, LV_ALIGN_BOTTOM_LEFT,  s_img_share },
        { -28, -39, LV_ALIGN_BOTTOM_RIGHT, s_img_award },
    };
    int g = 0;
    out->gesture[g++] = scr;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *cont = lv_obj_create(scr);
        lv_obj_remove_style_all(cont);
        lv_obj_set_size(cont, 70, 70);
        lv_obj_set_align(cont, s_corners[i].align);
        lv_obj_set_pos(cont, s_corners[i].x, s_corners[i].y);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *img = lv_image_create(cont);
        if (s_corners[i].img) lv_image_set_src(img, s_corners[i].img);
        else                  lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_align(img, LV_ALIGN_CENTER);
        lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
        lv_image_set_scale(img, 255);

        lv_obj_t *btn = lv_button_create(cont);
        lv_obj_set_size(btn, 70, 70);
        lv_obj_set_align(btn, LV_ALIGN_CENTER);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(btn, 500, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xF2F4FC), 0);
        lv_obj_set_style_bg_opa(btn, 255, 0);
        lv_obj_set_style_opa(btn, 30, 0);

        out->gesture[g++] = btn;   /* swipes starting on a button still work */
    }
    out->gesture[g] = NULL;

    /* ── Live clock (top-mid) ── */
    out->time = lv_label_create(scr);
    lv_obj_set_align(out->time, LV_ALIGN_TOP_MID);
    lv_obj_set_y(out->time, 35);
    lv_label_set_text(out->time, "--:--");
    lv_obj_set_style_text_color(out->time, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(out->time, 255, 0);
    lv_obj_set_style_text_font(out->time, &lv_font_montserrat_32, 0);

    /* ── Three concentric activity rings (move → exercise → stand) ──
     * Values start at 0; the app animates them up to the current value. */
    static const ring_cfg_t s_rings[3] = {
        { 355, 35, 0x2D0409, 0xF60000, 0 },   /* move     (outer) */
        { 270, 35, 0x032801, 0x00E000, 0 },   /* exercise (mid)   */
        { 185, 35, 0x002228, 0x00BDE6, 0 },   /* stand    (inner) */
    };
    for (int i = 0; i < 3; i++) {
        out->arcs[i] = activity_make_arc(scr, &s_rings[i]);
    }

    /* ── Ring symbols (move top, exercise mid, stand bottom) ─────── */
    struct {
        lv_coord_t y;
        const lv_image_dsc_t *img;
        int16_t rot;   /* 1/10 deg; 0 = no rotation */
    } s_syms[3] = {
        { -159, s_img_move,      0 },
        { -117, s_img_exercise,  0 },
        {  -73, s_img_move,   2700 },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *img = lv_image_create(scr);
        if (s_syms[i].img) lv_image_set_src(img, s_syms[i].img);
        else               lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_align(img, LV_ALIGN_CENTER);
        lv_obj_set_y(img, s_syms[i].y);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        if (s_syms[i].rot) lv_image_set_rotation(img, s_syms[i].rot);
    }

    out->scr = scr;
    return scr;
}

/* ══════════════════════════════════════════════════════════════════════
 * Detail pages (Move / Exercise / Stand) — one shared builder
 * ══════════════════════════════════════════════════════════════════════ */

lv_obj_t *activity_pages_build_detail(activity_page_t page, activity_page_ui_t *out)
{
    int idx = (page >= ACTIVITY_PAGE_MOVE && page <= ACTIVITY_PAGE_STAND)
              ? (int)(page - ACTIVITY_PAGE_MOVE) : 0;
    const detail_theme_t *th = &s_themes[idx];

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);   /* receive swipe events */
    lv_obj_set_style_bg_color(scr, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_opa(scr, 255, 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(th->bg_bottom), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

    int g = 0;
    out->gesture[g++] = scr;
    out->gesture[g] = NULL;

    /* ── Rings header (75x75, top-left): three nested arcs ────────── */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, 75, 75);
    lv_obj_set_pos(hdr, 28, 39);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    static const ring_cfg_t s_hdr_rings[3] = {
        { 75, 7, 0x2D0409, 0xF60000, 0 },   /* move     (outer) */
        { 57, 7, 0x032801, 0x00E000, 0 },   /* exercise (mid)   */
        { 38, 7, 0x002228, 0x00BDE6, 0 },   /* stand    (inner) */
    };
    for (int i = 0; i < 3; i++) {
        out->arcs[i] = activity_make_arc(hdr, &s_hdr_rings[i]);
    }

    /* ── Live clock (top-right) ── */
    out->time = lv_label_create(scr);
    lv_obj_set_align(out->time, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(out->time, -30, 36);
    lv_label_set_text(out->time, "--:--");
    lv_obj_set_style_text_color(out->time, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(out->time, 255, 0);
    lv_obj_set_style_text_font(out->time, &lv_font_montserrat_32, 0);

    /* ── Title (below clock) ── */
    out->title = lv_label_create(scr);
    lv_obj_set_align(out->title, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(out->title, -30, 76);
    lv_label_set_text(out->title, th->title);
    lv_obj_set_style_text_color(out->title, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_opa(out->title, 255, 0);
    lv_obj_set_style_text_font(out->title, &lv_font_montserrat_32, 0);

    /* ── Percentage (bottom-left) ── */
    out->percent = lv_label_create(scr);
    lv_obj_set_align(out->percent, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_pos(out->percent, 28, -71);
    lv_label_set_text(out->percent, "0%");
    lv_obj_set_style_text_color(out->percent, lv_color_hex(th->dim), 0);
    lv_obj_set_style_text_opa(out->percent, 255, 0);
    lv_obj_set_style_text_font(out->percent, &lv_font_montserrat_24, 0);

    /* ── Big value + unit (bottom-left) ──
     * The unit is pushed right up against the value (no gap, no overlap):
     * its x is derived from the value text width and re-derived every time
     * the value changes (activity_pages_set_detail_values). */
    lv_obj_t *vcont = lv_obj_create(scr);
    lv_obj_remove_style_all(vcont);
    lv_obj_set_size(vcont, 200, 50);
    lv_obj_set_align(vcont, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_pos(vcont, 28, -100);
    lv_obj_remove_flag(vcont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    out->value = lv_label_create(vcont);
    lv_label_set_text(out->value, "0/");
    lv_obj_set_style_text_color(out->value, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_opa(out->value, 255, 0);
    lv_obj_set_style_text_font(out->value, &lv_font_montserrat_48, 0);

    out->unit = lv_label_create(vcont);
    lv_obj_set_align(out->unit, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(out->unit, 0, 0);
    lv_label_set_text(out->unit, "");
    lv_obj_set_style_text_color(out->unit, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_opa(out->unit, 255, 0);
    lv_obj_set_style_text_font(out->unit, &lv_font_montserrat_24, 0);

    out->goal = th->goal;
    out->unit_suffix = th->unit_suffix;

    /* ── Chart (bottom-mid): 5 gridlines + 4 dynamic time labels ────
     * Labels / dotted markings / bars are (re)positioned by
     * activity_pages_update_chart, which always shows a rolling 24 h
     * window whose right edge is "now" (current 6 h section last). */
    lv_obj_t *chart = lv_obj_create(scr);
    lv_obj_remove_style_all(chart);
    lv_obj_set_size(chart, 350, 170);
    lv_obj_set_align(chart, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_pos(chart, 0, -188);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; i++) {               /* 0, 87, 174, 261, 348 */
        lv_obj_t *line = lv_obj_create(chart);
        lv_obj_set_size(line, 1, lv_pct(100));
        lv_obj_set_align(line, LV_ALIGN_BOTTOM_LEFT);
        lv_obj_set_x(line, i * 87);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(line, 5, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(line, 255, 0);
        lv_obj_set_style_opa(line, 45, 0);
    }

    for (int i = 0; i < 4; i++) {
        lv_obj_t *lbl = lv_label_create(chart);
        lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_LEFT);
        lv_obj_set_pos(lbl, 0, 2);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_color(lbl, lv_color_hex(th->dim), 0);
        lv_obj_set_style_text_opa(lbl, 255, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        out->chart_labels[i] = lbl;

        lv_obj_t *m = lv_label_create(scr);
        lv_obj_set_align(m, LV_ALIGN_CENTER);
        lv_obj_set_y(m, 22);
        lv_label_set_text(m, "............");
        lv_obj_set_style_text_color(m, lv_color_hex(th->accent), 0);
        lv_obj_set_style_text_opa(m, 255, 0);
        lv_obj_set_style_text_letter_space(m, -2, 0);
        lv_obj_set_style_text_line_space(m, 0, 0);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_38, 0);
        lv_obj_set_style_opa(m, 89, 0);
        out->chart_marks[i] = m;
    }

    /* ── Hourly bars (top-mid) — every bar gradient, no dim bars ──── */
    lv_obj_t *bcont = lv_obj_create(scr);
    lv_obj_remove_style_all(bcont);
    lv_obj_set_size(bcont, 350, 120);
    lv_obj_set_align(bcont, LV_ALIGN_TOP_MID);
    lv_obj_set_pos(bcont, 0, 160);
    lv_obj_remove_flag(bcont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    out->bar_count = (th->bar_count > ACTIVITY_MAX_BARS) ? ACTIVITY_MAX_BARS : th->bar_count;
    for (int i = 0; i < th->bar_count; i++) {
        out->bar_hour[i] = th->bar_hour[i];
        lv_obj_t *bar = lv_obj_create(bcont);
        lv_obj_set_size(bar, 5, lv_pct(th->bar_h[i]));
        lv_obj_set_align(bar, LV_ALIGN_BOTTOM_LEFT);
        lv_obj_set_pos(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(th->bar_grad_top), 0);
        lv_obj_set_style_bg_opa(bar, 255, 0);
        lv_obj_set_style_bg_grad_color(bar, lv_color_hex(th->bar_grad_bottom), 0);
        lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
        out->bars[i] = bar;
    }

    out->scr = scr;
    return scr;
}

/* ══════════════════════════════════════════════════════════════════════
 * Chart refresh — rolling 24 h window ending at "now"
 *
 * The chart always ends at the current time.  Its four 6-hour sections
 * are (left → right): B-18h, B-12h, B-6h and the CURRENT section B..now,
 * where B is the start of the current 6 h block (0/6/12/18).  So the
 * rightmost label always marks the current section and the label order
 * changes as the day advances — the hardcoded 12AM/6AM/12PM/6PM order
 * from the SquareLine design only held for one moment of the day.
 * ══════════════════════════════════════════════════════════════════════ */
static void activity_fmt_section_label(int s24, char *buf, size_t n)
{
    int h = s24 % 12;
    if (h == 0) h = 12;
    snprintf(buf, n, "%d%s", h, (s24 < 12) ? "AM" : "PM");
}

void activity_pages_update_chart(activity_page_ui_t *ui, float now_frac)
{
    if (!ui->scr) return;

    /* Start of the current 6 h block (0, 6, 12 or 18). */
    int b = ((int)(now_frac / 6.0f)) * 6;
    if (b > 18) b = 18;

    for (int i = 0; i < 4; i++) {
        int s = (b - 18) + 6 * i;          /* B-18, B-12, B-6, B */
        s %= 24;
        if (s < 0) s += 24;

        /* x on the 350 px chart = ((S - now) mod 24) / 24 * 350. */
        float h = fmodf(s - now_frac, 24.0f);
        if (h < 0) h += 24.0f;
        int x = (int)(h / 24.0f * 350.0f);

        char buf[8];
        activity_fmt_section_label(s, buf, sizeof(buf));
        lv_label_set_text(ui->chart_labels[i], buf);
        lv_obj_set_x(ui->chart_labels[i], x);

        /* Dotted marking tracks the label (chart x → screen centre-rel). */
        lv_obj_set_x(ui->chart_marks[i], x - 139);
    }

    /* Bars: same rolling mapping from each bar's absolute hour-of-day. */
    for (int i = 0; i < ui->bar_count; i++) {
        float h = fmodf(ui->bar_hour[i] - now_frac, 24.0f);
        if (h < 0) h += 24.0f;
        int x = (int)(h / 24.0f * 350.0f);
        lv_obj_set_x(ui->bars[i], x);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Live metric refresh — big value, goal+unit, percentage and header rings
 * ══════════════════════════════════════════════════════════════════════ */
void activity_pages_set_detail_values(activity_page_ui_t *ui, int value, int goal,
                                      int percent, const int ring_pct[3])
{
    if (!ui->scr) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d/", value);
    lv_label_set_text(ui->value, buf);

    snprintf(buf, sizeof(buf), "%d\n%s", goal, ui->unit_suffix);
    lv_label_set_text(ui->unit, buf);

    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(ui->percent, buf);

    /* Keep the unit glued to the value — no gap, no overlap. */
    const char *v = lv_label_get_text(ui->value);
    lv_point_t _txt_w;
    lv_text_get_size(&_txt_w, v, &lv_font_montserrat_48, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t vw = _txt_w.x;
    lv_obj_set_x(ui->unit, vw + 6);

    for (int i = 0; i < 3; i++) {
        lv_arc_set_value(ui->arcs[i], ring_pct[i]);
    }
}
