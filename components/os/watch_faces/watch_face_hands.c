/**
 * @file watch_face_hands.c
 * @brief "Analog Hands" watch face — Apple-style analog face built from the
 *        cropped 42mm hand alpha masks.
 *
 * Assets (tools/build_hands_face.py) live in
 *   spiffs/usr/share/watchfaces/hands/layers/<role>.bin
 * Each .bin is the raw A8 alpha mask (1 B/px) of a crop of the original
 * 314x392 PNG; the rotation pivot is the original canvas centre (156.5,195.5)
 * expressed inside the crop (see hands_assets_gen.h).  Masks are loaded once
 * into PSRAM at face create via app_img_load_raw() (the same path the Flux
 * digits use) and drawn with lv_image_set_rotation()/pivot.  Every mask is
 * recoloured at COVER with lv_image_set_style_image_recolor(), so a colour
 * slot tints the whole shape — the "one signal colour" model.
 *
 * Z-order (bottom -> top): hour shadow/hand/inlay, minute shadow/hand/inlay,
 * second shadow/hand, centre pin.  Shadows are recoloured black (their alpha
 * makes them soft); on a pure-black background they are invisible, and they
 * appear once the background colour is lightened.
 *
 * Colours (4 slots, like Flux but 4 not 2), persisted in g_config:
 *   slot 0 = background + centre pin   (hands_face_bg)
 *   slot 1 = hour hand + inlay         (hands_face_hour)
 *   slot 2 = minute hand + inlay       (hands_face_min)
 *   slot 3 = second hand               (hands_face_sec)
 *
 * The full-screen active face lives in s_full (registry instance); the picker
 * / editor use hands_preview_create() to host a live scaled copy.
 *
 * AOD: while Always-On Display is active the AMOLED is in MIPI DCS Idle mode
 * (~1 Hz self refresh), so the face TICKS once per second there (second hand
 * snaps to its second) instead of running the smooth 1 s sweep — a sweep
 * animation would repaint at full render rate and defeat the low-power idle.
 */
#include "watch_face.h"
#include "watch_face_hands.h"
#include "hands_assets_gen.h"
#include "app_config.h"
#include "app_image_loader.h"
#include "app_power.h"          /* AOD state — see hands_set_time() */
#include "hw_rtc.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WF_HANDS";

/* ── Geometry (410x502 screen; symmetric integer centre) ───────────── */
#define HANDS_SCR_W   410
#define HANDS_SCR_H   502
#define HANDS_CX      205
#define HANDS_CY      251
#define HANDS_DIR     "/spiffs/usr/share/watchfaces/hands/layers"

/* Default preview pose used when no real time is available. */
#define HANDS_DEF_H   10
#define HANDS_DEF_M   8

/* ── One runtime instance ──────────────────────────────────────────── */
typedef struct hands_inst {
    lv_obj_t        *root;      /* full-screen screen, or preview container */
    lv_obj_t        *clock;     /* children container (AOD pixel shift)     */
    lv_obj_t        *imgs[HANDS_ASSET_COUNT];
    lv_image_dsc_t  *dsc[HANDS_ASSET_COUNT];   /* owned A8 masks (PSRAM)    */
    bool             step;      /* preview: position hands, never sweep    */
    int32_t          sec_ang;   /* last second-hand angle (0.1 deg)         */
    bool             sweeping;  /* a sweep anim is armed                    */
} hands_inst_t;

/* Active (registry) instance. */
static hands_inst_t s_full;

/* ── Colour application ─────────────────────────────────────────────── */

static void img_recolor(lv_obj_t *img, uint32_t rgb)
{
    if (!img || !lv_obj_is_valid(img)) return;
    lv_obj_set_style_image_recolor(img, lv_color_hex(rgb), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
}

/* Apply the four slot colours.  A8 masks carry no colour, so EVERY layer is
 * recoloured: bg -> screen + pin, hour/min -> hand+inlay, shadows -> black. */
static void apply_colors(hands_inst_t *f, uint32_t bg, uint32_t hour,
                         uint32_t min, uint32_t sec)
{
    if (f->root && lv_obj_is_valid(f->root)) {
        lv_obj_set_style_bg_color(f->root, lv_color_hex(bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(f->root, LV_OPA_COVER, LV_PART_MAIN);
    }
    img_recolor(f->imgs[HANDS_ASSET_H_SHADOW], 0x000000);
    img_recolor(f->imgs[HANDS_ASSET_H_HAND],   hour);
    img_recolor(f->imgs[HANDS_ASSET_H_INLAY],  hour);
    img_recolor(f->imgs[HANDS_ASSET_M_SHADOW], 0x000000);
    img_recolor(f->imgs[HANDS_ASSET_M_HAND],   min);
    img_recolor(f->imgs[HANDS_ASSET_M_INLAY],  min);
    img_recolor(f->imgs[HANDS_ASSET_S_SHADOW], 0x000000);
    img_recolor(f->imgs[HANDS_ASSET_S_HAND],   sec);
    img_recolor(f->imgs[HANDS_ASSET_PIN_SMALL], bg);
}

/* ── Asset loading (raw A8 alpha masks into PSRAM) ─────────────────── */

/* Load <DIR>/<file> via app_img_load_raw (Flux-digit path: fopen + one PSRAM
 * alloc).  A8 rows are packed at 1 B/px, so stride = width. */
static bool asset_load(hands_asset_id_t id, lv_image_dsc_t **out)
{
    const hands_asset_meta_t *m = &hands_asset_meta[id];
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", HANDS_DIR, m->file);

    lv_image_dsc_t *d = app_img_load_raw(path, m->w, m->h,
                                         LV_COLOR_FORMAT_A8);
    if (!d) {
        ESP_LOGE(TAG, "load %s failed — run tools/build_hands_face.py", path);
        return false;
    }
    d->header.stride = m->w;          /* A8: 1 byte per pixel, packed rows */
    *out = d;
    return true;
}

static void inst_free_assets(hands_inst_t *f)
{
    for (int i = 0; i < HANDS_ASSET_COUNT; i++) {
        if (f->dsc[i]) {
            app_img_free(f->dsc[i]);
            f->dsc[i] = NULL;
        }
    }
}

/* ── Geometry / rotation ───────────────────────────────────────────── */

/* Add an asset as an lv_image anchored so its pivot sits at the face centre. */
static lv_obj_t *add_asset(hands_inst_t *f, lv_obj_t *parent,
                           hands_asset_id_t id)
{
    const hands_asset_meta_t *m = &hands_asset_meta[id];
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (f->dsc[id]) {
        lv_image_set_src(img, f->dsc[id]);
    } else {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_set_pivot(img, m->pivot_x, m->pivot_y);
    lv_obj_set_pos(img, HANDS_CX - m->pivot_x, HANDS_CY - m->pivot_y);
    f->imgs[id] = img;
    return img;
}

/* Rotate a hand group (shadow + hand [+ inlay]) about the shared pivot. */
static void group_rotate(hands_inst_t *f, hands_asset_id_t shadow,
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

/* Second-hand sweep exec — sets the second shadow + hand angle. */
static void sec_sweep_exec(void *var, int32_t v)
{
    hands_inst_t *f = (hands_inst_t *)var;
    if (!f) return;
    if (v < 0) v = 0;
    if (v > 3600) v = 3600;              /* 3600 == 12 o'clock (== 0) */
    f->sec_ang = v % 3600;               /* keep the start point normalized */
    group_rotate(f, HANDS_ASSET_S_SHADOW, HANDS_ASSET_S_HAND,
                 HANDS_ASSET_COUNT, v);   /* COUNT = no inlay */
}

/* Position all hands from a (local 12h) time.  animate: sweep the second
 * hand over ~1 s (active face); otherwise snap directly (previews). */
static void hands_set_time(hands_inst_t *f, int h12, int m, int s, bool animate)
{
    if (!f || !f->root || !lv_obj_is_valid(f->root)) return;

    if (h12 < 0) h12 = HANDS_DEF_H;         /* fallback pose 10:08 */
    if (m < 0)   m   = HANDS_DEF_M;
    if (s < 0)   s   = 0;

    int32_t h_ang = ((h12 % 12) * 300 + m * 5) % 3600;
    int32_t m_ang = (m * 60 + s) % 3600;
    int32_t s_ang = (s % 60) * 60;

    group_rotate(f, HANDS_ASSET_H_SHADOW, HANDS_ASSET_H_HAND,
                 HANDS_ASSET_H_INLAY, h_ang);
    group_rotate(f, HANDS_ASSET_M_SHADOW, HANDS_ASSET_M_HAND,
                 HANDS_ASSET_M_INLAY, m_ang);

    if (!animate || f->step) {
        /* 1 Hz AOD / preview: snap the second hand to its target.  Kill any
         * in-flight sweep first so a leftover animation can't keep repainting
         * the hand after AOD (or a preview) takes over. */
        lv_anim_del(f, sec_sweep_exec);
        sec_sweep_exec(f, s_ang);
        return;
    }

    /* Smooth sweep from the current angle to the new target (~1 s).  When
     * the second hand crosses 12 o'clock (59 -> 0) keep moving FORWARD
     * through 3600 rather than animating a full backward spin. */
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

/* ── Time dispatch ─────────────────────────────────────────────────── */

static void inst_update_time(hands_inst_t *f, const char *time_str,
                             const char *date_str)
{
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

    /* Live active face sweeps the second hand; preview instances snap.
     * In AOD the panel idle-refreshes at ~1 Hz, so only TICK once per second
     * there — a continuous lv_anim would keep pushing frames at the full
     * render rate and defeat the low-power idle mode. */
    bool sweep = !f->step && !app_power_get_aod_active();
    hands_set_time(f, h, m, s, sweep);
}

/* ── Instance build / destroy ──────────────────────────────────────── */

static bool inst_build_assets(hands_inst_t *f)
{
    for (hands_asset_id_t id = 0; id < HANDS_ASSET_COUNT; id++) {
        if (!asset_load(id, &f->dsc[id])) continue;
    }
    bool any = false;
    for (int i = 0; i < HANDS_ASSET_COUNT; i++) any |= (f->dsc[i] != NULL);
    return any;
}

static lv_obj_t *inst_build_root(hands_inst_t *f, lv_obj_t *parent)
{
    f->root = lv_obj_create(parent);
    lv_obj_remove_flag(f->root, LV_OBJ_FLAG_SCROLLABLE);
    if (parent == NULL) {
        /* Screen: full size + black. */
        lv_obj_set_size(f->root, HANDS_SCR_W, HANDS_SCR_H);
        lv_obj_set_pos(f->root, 0, 0);
    } else {
        lv_obj_remove_flag(f->root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(f->root, HANDS_SCR_W, HANDS_SCR_H);
        lv_obj_set_align(f->root, LV_ALIGN_CENTER);
    }
    lv_obj_set_style_bg_color(f->root, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f->root, LV_OPA_COVER, LV_PART_MAIN);

    /* All moving parts hang off one container so AOD pixel-shift can move
     * the whole clock face by (dx, dy) with a single lv_obj_set_pos(). */
    f->clock = lv_obj_create(f->root);
    lv_obj_remove_style_all(f->clock);
    lv_obj_set_size(f->clock, HANDS_SCR_W, HANDS_SCR_H);
    lv_obj_set_pos(f->clock, 0, 0);
    lv_obj_remove_flag(f->clock, LV_OBJ_FLAG_CLICKABLE |
                                LV_OBJ_FLAG_SCROLLABLE);

    /* bottom -> top */
    add_asset(f, f->clock, HANDS_ASSET_H_SHADOW);
    add_asset(f, f->clock, HANDS_ASSET_H_HAND);
    add_asset(f, f->clock, HANDS_ASSET_H_INLAY);
    add_asset(f, f->clock, HANDS_ASSET_M_SHADOW);
    add_asset(f, f->clock, HANDS_ASSET_M_HAND);
    add_asset(f, f->clock, HANDS_ASSET_M_INLAY);
    add_asset(f, f->clock, HANDS_ASSET_S_SHADOW);
    add_asset(f, f->clock, HANDS_ASSET_S_HAND);
    add_asset(f, f->clock, HANDS_ASSET_PIN_SMALL);

    return f->root;
}

static void inst_destroy(hands_inst_t *f)
{
    if (!f) return;
    if (f->root) {
        lv_anim_del(f, sec_sweep_exec);
        lv_obj_delete(f->root);
    }
    f->root = NULL;
    f->clock = NULL;
    memset(f->imgs, 0, sizeof(f->imgs));
    inst_free_assets(f);
    f->step = false;
    f->sec_ang = 0;
    f->sweeping = false;
}

/* ── watch_face_t implementation (full-screen active face) ────────── */

static lv_obj_t *hands_create(void)
{
    hands_inst_t *f = &s_full;
    if (f->root) return f->root;         /* idempotent until destroy() */

    if (!inst_build_assets(f)) {
        ESP_LOGE(TAG, "no hand masks found — run tools/build_hands_face.py");
    }
    inst_build_root(f, NULL);
    apply_colors(f, g_config.hands_face_bg, g_config.hands_face_hour,
                 g_config.hands_face_min, g_config.hands_face_sec);

    /* Initial pose (10:08), snapped — no sweep on entry. */
    hands_set_time(f, HANDS_DEF_H, HANDS_DEF_M, -1, false);
    ESP_LOGI(TAG, "Analog Hands face ready");
    return f->root;
}

static void hands_destroy(void)
{
    inst_destroy(&s_full);
}

static void hands_update_time(const char *time_str, const char *date_str)
{
    inst_update_time(&s_full, time_str, date_str);
}

static void hands_update_battery(int pct, bool charging, bool vbus_present)
{
    (void)pct; (void)charging; (void)vbus_present;   /* no battery UI */
}

static int s_aod_dx = 0, s_aod_dy = 0;

static void hands_aod_shift(int dx, int dy)
{
    s_aod_dx = dx;
    s_aod_dy = dy;
    if (s_full.clock && lv_obj_is_valid(s_full.clock)) {
        lv_obj_set_pos(s_full.clock, dx, dy);
    }
}

const watch_face_t watch_face_hands = {
    .name           = "Analog Hands",
    .id             = WATCH_FACE_HANDS,
    .create         = hands_create,
    .destroy        = hands_destroy,
    .update_time    = hands_update_time,
    .update_battery = hands_update_battery,
    .aod_pixel_shift = hands_aod_shift,
};

/* ── Editor / preview API ──────────────────────────────────────────── */

hands_inst_t *hands_preview_create(lv_obj_t *parent)
{
    if (!parent) return NULL;
    hands_inst_t *f = (hands_inst_t *)calloc(1, sizeof(hands_inst_t));
    if (!f) return NULL;
    f->step = true;                     /* previews snap, never sweep */
    if (!inst_build_assets(f)) {
        ESP_LOGE(TAG, "preview: no hand masks — run build_hands_face.py");
    }
    inst_build_root(f, parent);
    apply_colors(f, g_config.hands_face_bg, g_config.hands_face_hour,
                 g_config.hands_face_min, g_config.hands_face_sec);
    hands_set_time(f, HANDS_DEF_H, HANDS_DEF_M, -1, false);
    return f;
}

void hands_preview_destroy(hands_inst_t *f)
{
    if (!f) return;
    inst_destroy(f);
    free(f);
}

lv_obj_t *hands_preview_root(hands_inst_t *f)
{
    return f ? f->root : NULL;
}

void hands_preview_set_colors(hands_inst_t *f, uint32_t bg, uint32_t hour,
                              uint32_t min, uint32_t sec)
{
    if (!f) return;
    apply_colors(f, bg, hour, min, sec);
}

void hands_preview_update(hands_inst_t *f, const char *time_str,
                          const char *date_str)
{
    if (!f) return;
    inst_update_time(f, time_str, date_str);
}
