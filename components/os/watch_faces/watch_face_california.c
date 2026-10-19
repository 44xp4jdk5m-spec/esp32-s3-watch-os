/**
 * @file watch_face_california.c
 * @brief "California" watch face — Apple-style fullscreen dial + the analog
 *        hands component (same 42mm hand masks / pivot / sweep as Analog Hands).
 *
 * Layout (bottom -> top), all inside one "clock" container anchored at the
 * screen centre (205,251) so AOD pixel-shift moves the whole face:
 *   1. California dial (full-screen A8 masks, recoloured):
 *        small_ticks (gray)  <- minute ring
 *        big_ticks   (white) <- 12 hour marks
 *        symbol_california (white)
 *   2. hour shadow / WHITE hour hand / BLACK inlay
 *   3. minute shadow / WHITE minute hand / BLACK inlay
 *   4. second shadow / red second hand (smooth 1 s sweep; 1 Hz tick in AOD)
 *   5. black centre pin
 * Background is black.
 *
 * Dials are baked to 410x502 raw A8 masks by tools/build_california_face.py and
 * loaded via app_img_load_raw() (no codec).  Fixed palette (not editor-driven).
 */
#include "watch_face.h"
#include "hands_assets_gen.h"
#include "app_image_loader.h"
#include "app_power.h"          /* AOD state — 1 Hz tick in AOD */
#include "hw_rtc.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WF_CALI";

/* ── Geometry ──────────────────────────────────────────────────────── */
#define CAL_SCR_W     410
#define CAL_SCR_H     502
#define CAL_CX        205
#define CAL_CY        251
#define CAL_DIR       "/spiffs/usr/share/watchfaces/california"
#define HANDS_DIR     "/spiffs/usr/share/watchfaces/hands/layers"

#define CAL_DIAL_N    3
#define CAL_DIAL_W    410
#define CAL_DIAL_H    502

static const char *s_dial_file[CAL_DIAL_N] = {
    "small_ticks.bin", "big_ticks.bin", "symbol_california.bin",
};

/* Fixed palette. */
#define COL_BG        0x000000
#define COL_SMALL     0x9A9A9A   /* minute ring (gray)      */
#define COL_BIG       0xFFFFFF   /* hour marks (white)      */
#define COL_SYMBOL    0xFFFFFF   /* California symbol       */
#define COL_HAND      0xFFFFFF   /* hour + minute hands     */
#define COL_INLAY     0x000000   /* hand inlays             */
#define COL_SEC       0xFF3B30   /* second hand             */
#define COL_SHADOW    0x000000
#define COL_PIN       0x000000

#define CAL_DEF_H     10
#define CAL_DEF_M     8

/* ── Instance ──────────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t        *root;
    lv_obj_t        *clock;
    lv_obj_t        *imgs[HANDS_ASSET_COUNT];     /* hand masks */
    lv_image_dsc_t  *dsc[HANDS_ASSET_COUNT];
    lv_obj_t        *dial[CAL_DIAL_N];
    lv_image_dsc_t  *dial_dsc[CAL_DIAL_N];
    int32_t          sec_ang;
    bool             sweeping;
} cal_inst_t;

static cal_inst_t s_cal;

/* ── Colour ────────────────────────────────────────────────────────── */
static void img_recolor(lv_obj_t *img, uint32_t rgb)
{
    if (!img || !lv_obj_is_valid(img)) return;
    lv_obj_set_style_image_recolor(img, lv_color_hex(rgb), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
}

static void apply_colors(cal_inst_t *f)
{
    if (f->root && lv_obj_is_valid(f->root)) {
        lv_obj_set_style_bg_color(f->root, lv_color_hex(COL_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(f->root, LV_OPA_COVER, LV_PART_MAIN);
    }
    img_recolor(f->dial[0], COL_SMALL);
    img_recolor(f->dial[1], COL_BIG);
    img_recolor(f->dial[2], COL_SYMBOL);

    img_recolor(f->imgs[HANDS_ASSET_H_SHADOW],  COL_SHADOW);
    img_recolor(f->imgs[HANDS_ASSET_H_HAND],    COL_HAND);
    img_recolor(f->imgs[HANDS_ASSET_H_INLAY],   COL_INLAY);
    img_recolor(f->imgs[HANDS_ASSET_M_SHADOW],  COL_SHADOW);
    img_recolor(f->imgs[HANDS_ASSET_M_HAND],    COL_HAND);
    img_recolor(f->imgs[HANDS_ASSET_M_INLAY],   COL_INLAY);
    img_recolor(f->imgs[HANDS_ASSET_S_SHADOW],  COL_SHADOW);
    img_recolor(f->imgs[HANDS_ASSET_S_HAND],    COL_SEC);
    img_recolor(f->imgs[HANDS_ASSET_PIN_SMALL], COL_PIN);
}

/* ── Asset loading (raw A8 alpha masks) ────────────────────────────── */
static bool load_a8(const char *dir, const char *file, int w, int h,
                    lv_image_dsc_t **out)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    lv_image_dsc_t *d = app_img_load_raw(path, w, h, LV_COLOR_FORMAT_A8);
    if (!d) {
        ESP_LOGE(TAG, "load %s failed — run the *_face build script", path);
        return false;
    }
    d->header.stride = w;              /* A8: 1 B/px, packed rows */
    *out = d;
    return true;
}

static void free_dsc(lv_image_dsc_t **dsc, int n)
{
    for (int i = 0; i < n; i++) {
        if (dsc[i]) {
            app_img_free(dsc[i]);
            dsc[i] = NULL;
        }
    }
}

/* ── Rotation helpers ──────────────────────────────────────────────── */
static void add_hand(cal_inst_t *f, lv_obj_t *parent, hands_asset_id_t id)
{
    const hands_asset_meta_t *m = &hands_asset_meta[id];
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (f->dsc[id]) lv_image_set_src(img, f->dsc[id]);
    else            lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_pivot(img, m->pivot_x, m->pivot_y);
    lv_obj_set_pos(img, CAL_CX - m->pivot_x, CAL_CY - m->pivot_y);
    f->imgs[id] = img;
}

static void add_dial(cal_inst_t *f, lv_obj_t *parent, int i)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (f->dial_dsc[i]) lv_image_set_src(img, f->dial_dsc[i]);
    else                lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(img, 0, 0);
    f->dial[i] = img;
}

static void group_rotate(cal_inst_t *f, hands_asset_id_t shadow,
                         hands_asset_id_t hand, hands_asset_id_t inlay,
                         int32_t ang)
{
    if (f->imgs[shadow] && lv_obj_is_valid(f->imgs[shadow]))
        lv_image_set_rotation(f->imgs[shadow], ang);
    if (f->imgs[hand] && lv_obj_is_valid(f->imgs[hand]))
        lv_image_set_rotation(f->imgs[hand], ang);
    if (inlay < HANDS_ASSET_COUNT && f->imgs[inlay] &&
        lv_obj_is_valid(f->imgs[inlay]))
        lv_image_set_rotation(f->imgs[inlay], ang);
}

static void sec_sweep_exec(void *var, int32_t v)
{
    cal_inst_t *f = (cal_inst_t *)var;
    if (!f) return;
    if (v < 0) v = 0;
    if (v > 3600) v = 3600;
    f->sec_ang = v % 3600;
    group_rotate(f, HANDS_ASSET_S_SHADOW, HANDS_ASSET_S_HAND,
                 HANDS_ASSET_COUNT, v);
}

static void set_time(cal_inst_t *f, int h12, int m, int s, bool animate)
{
    if (!f || !f->root || !lv_obj_is_valid(f->root)) return;
    if (h12 < 0) h12 = CAL_DEF_H;
    if (m < 0)   m   = CAL_DEF_M;
    if (s < 0)   s   = 0;

    int32_t h_ang = ((h12 % 12) * 300 + m * 5) % 3600;
    int32_t m_ang = (m * 60 + s) % 3600;
    int32_t s_ang = (s % 60) * 60;

    group_rotate(f, HANDS_ASSET_H_SHADOW, HANDS_ASSET_H_HAND,
                 HANDS_ASSET_H_INLAY, h_ang);
    group_rotate(f, HANDS_ASSET_M_SHADOW, HANDS_ASSET_M_HAND,
                 HANDS_ASSET_M_INLAY, m_ang);

    if (!animate) {
        lv_anim_del(f, sec_sweep_exec);
        sec_sweep_exec(f, s_ang);
        return;
    }
    int32_t start = f->sec_ang;
    int32_t end   = s_ang;
    if (end < start) end += 3600;
    lv_anim_del(f, sec_sweep_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, f);
    lv_anim_set_exec_cb(&a, sec_sweep_exec);
    lv_anim_set_values(&a, start, end);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
    f->sweeping = true;
}

/* ── Build / destroy ───────────────────────────────────────────────── */
static bool build_assets(cal_inst_t *f)
{
    bool any = false;
    for (hands_asset_id_t id = 0; id < HANDS_ASSET_COUNT; id++) {
        const hands_asset_meta_t *m = &hands_asset_meta[id];
        if (load_a8(HANDS_DIR, m->file, m->w, m->h, &f->dsc[id])) any = true;
    }
    for (int i = 0; i < CAL_DIAL_N; i++) {
        if (load_a8(CAL_DIR, s_dial_file[i], CAL_DIAL_W, CAL_DIAL_H,
                    &f->dial_dsc[i])) any = true;
    }
    return any;
}

static lv_obj_t *build_root(cal_inst_t *f)
{
    f->root = lv_obj_create(NULL);
    lv_obj_remove_flag(f->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(f->root, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f->root, LV_OPA_COVER, LV_PART_MAIN);

    f->clock = lv_obj_create(f->root);
    lv_obj_remove_style_all(f->clock);
    lv_obj_set_size(f->clock, CAL_SCR_W, CAL_SCR_H);
    lv_obj_set_pos(f->clock, 0, 0);
    lv_obj_remove_flag(f->clock, LV_OBJ_FLAG_CLICKABLE |
                                LV_OBJ_FLAG_SCROLLABLE);

    /* dials */
    for (int i = 0; i < CAL_DIAL_N; i++) add_dial(f, f->clock, i);
    /* hands */
    add_hand(f, f->clock, HANDS_ASSET_H_SHADOW);
    add_hand(f, f->clock, HANDS_ASSET_H_HAND);
    add_hand(f, f->clock, HANDS_ASSET_H_INLAY);
    add_hand(f, f->clock, HANDS_ASSET_M_SHADOW);
    add_hand(f, f->clock, HANDS_ASSET_M_HAND);
    add_hand(f, f->clock, HANDS_ASSET_M_INLAY);
    add_hand(f, f->clock, HANDS_ASSET_S_SHADOW);
    add_hand(f, f->clock, HANDS_ASSET_S_HAND);
    add_hand(f, f->clock, HANDS_ASSET_PIN_SMALL);

    return f->root;
}

static void destroy(cal_inst_t *f)
{
    if (!f) return;
    if (f->root) {
        lv_anim_del(f, sec_sweep_exec);
        lv_obj_delete(f->root);
    }
    f->root = NULL;
    f->clock = NULL;
    memset(f->imgs, 0, sizeof(f->imgs));
    memset(f->dial, 0, sizeof(f->dial));
    free_dsc(f->dsc, HANDS_ASSET_COUNT);
    free_dsc(f->dial_dsc, CAL_DIAL_N);
    f->sec_ang = 0;
    f->sweeping = false;
}

/* ── watch_face_t ──────────────────────────────────────────────────── */
static lv_obj_t *cali_create(void)
{
    cal_inst_t *f = &s_cal;
    if (f->root) return f->root;
    if (!build_assets(f)) {
        ESP_LOGE(TAG, "no assets — run tools/build_california_face.py");
    }
    build_root(f);
    apply_colors(f);
    set_time(f, CAL_DEF_H, CAL_DEF_M, -1, false);
    ESP_LOGI(TAG, "California face ready");
    return f->root;
}

static void cali_destroy(void)          { destroy(&s_cal); }

static void cali_update_time(const char *time_str, const char *date_str)
{
    cal_inst_t *f = &s_cal;
    (void)date_str;
    int h = -1, m = -1, s = -1;
    if (time_str && time_str[0] >= '0' && time_str[0] <= '9' &&
        time_str[1] >= '0' && time_str[1] <= '9' &&
        time_str[3] >= '0' && time_str[3] <= '9' &&
        time_str[4] >= '0' && time_str[4] <= '9') {
        h = (time_str[0] - '0') * 10 + (time_str[1] - '0');
        m = (time_str[3] - '0') * 10 + (time_str[4] - '0');
    }
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) == ESP_OK) s = rt.sec;

    /* Sweep when fully awake; in AOD the panel idle-refreshes at ~1 Hz so
     * only tick once per second there. */
    bool sweep = !app_power_get_aod_active();
    set_time(f, h, m, s, sweep);
}

static void cali_update_battery(int pct, bool charging, bool vbus_present)
{
    (void)pct; (void)charging; (void)vbus_present;   /* no battery UI */
}

static int s_aod_dx = 0, s_aod_dy = 0;

static void cali_aod_shift(int dx, int dy)
{
    s_aod_dx = dx;
    s_aod_dy = dy;
    if (s_cal.clock && lv_obj_is_valid(s_cal.clock)) {
        lv_obj_set_pos(s_cal.clock, dx, dy);
    }
}

const watch_face_t watch_face_california = {
    .name            = "California",
    .id              = WATCH_FACE_CALIFORNIA,
    .create          = cali_create,
    .destroy         = cali_destroy,
    .update_time     = cali_update_time,
    .update_battery  = cali_update_battery,
    .aod_pixel_shift = cali_aod_shift,
};
