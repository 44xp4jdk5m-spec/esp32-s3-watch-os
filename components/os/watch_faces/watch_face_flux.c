/**
 * @file watch_face_flux.c
 * @brief "Flux" watch face — Apple-watch-style animated face.
 *
 *        Layout adopted from SquareLine Studio's ui_flux.c:
 *          - ui_panel0 : full-width blue bar (0x567CFF), top-mid
 *          - ui_panel1 : full-width green bar (0x00E374), bottom-mid
 *          - 8 digit containers (4 per panel) hosting stretched digit
 *            images rendered from Noto Sans Bold (solid, no AA):
 *              hourxx   = hour tens
 *              hourx    = hour ones
 *              minutex  = minute tens
 *              minutexx = minute ones
 *            `_0` = white digits on the blue panel,
 *            `_1` = black digits on the green panel.
 *
 *        Panels are complementary (they always meet): at sec=0 green is
 *        0 high and blue fills the screen; as the seconds rise green
 *        grows from the bottom while blue recedes from the top; at
 *        sec=60 green fills the screen and the cycle restarts.  Digits
 *        are clipped to their panel, so each digit flips colour as the
 *        boundary sweeps over it.
 */
#include "watch_face.h"
#include "app_image_loader.h"
#include "app_config.h"  /* saved Flux colors (editor) */
#include "hw_rtc.h"      /* live seconds */
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Geometry ──────────────────────────────────────────────────────── */
#define FLUX_COLOR_TOP   0xff4800  /* default top panel color */
#define FLUX_COLOR_BOT   0xe1ff00  /* default bottom panel color */
#define FLUX_SCREEN_W    410
#define FLUX_SCREEN_H    502
#define FLUX_STEP        (FLUX_SCREEN_H / 60.0f)   /* px per second */

/* One digit box from SquareLine ui_flux.c. */
typedef struct {
    lv_coord_t w, h;
    int32_t x, y;
    lv_align_t align;
} flux_digit_cfg_t;

/* Index 0-3 = top panel (_0), index 4-7 = bottom panel (_1). */
static const flux_digit_cfg_t s_digit_cfg[8] = {
    { 113, 246,   37,   40, LV_ALIGN_DEFAULT },         /* hourxx_0  */
    { 208, 143,  -36,   40, LV_ALIGN_TOP_RIGHT },       /* hourx_0   */
    { 208, 263,  -36,  200, LV_ALIGN_TOP_RIGHT },       /* minutex_0 */
    { 113, 162,   37,  301, LV_ALIGN_DEFAULT },         /* minutexx_0 */
    { 113, 246,   37, -216, LV_ALIGN_BOTTOM_LEFT },     /* hourxx_1  */
    { 208, 143,  -36, -319, LV_ALIGN_BOTTOM_RIGHT },    /* hourx_1   */
    { 208, 263,  -36,  -39, LV_ALIGN_BOTTOM_RIGHT },    /* minutex_1 */
    { 113, 162,   37,  -39, LV_ALIGN_BOTTOM_LEFT },     /* minutexx_1 */
};

/* ── Per-instance state ──────────────────────────────────────────────
 * Flux can exist as the active watch-face screen AND as the editor's
 * live scaled-down preview at the same time, so all state lives in an
 * instance instead of file-scope statics.  The watch_face_t API drives
 * the single "active" instance s_flux. */
typedef struct flux_inst {
    lv_obj_t         *root;      /* the screen (active) or container (preview) */
    lv_obj_t         *panel0;    /* top panel  */
    lv_obj_t         *panel1;    /* bottom panel */
    lv_obj_t         *imgs[8];   /* digit images */
    lv_image_dsc_t   *digits[10];/* digit bitmaps (loaded from SPIFFS) */
    bool              step_sweep;/* true: snap panel heights (editor preview, cheap) */
} flux_inst_t;

static flux_inst_t s_flux;       /* instance backing the watch_face_t API */

/* Digit bitmaps (A8) are loaded from SPIFFS into PSRAM when this face is
 * shown and freed on destroy — see app_image_loader.h. */
static const struct { uint16_t w, h; } s_digit_dims[10] = {
    { 171, 288 }, {  52, 288 }, { 171, 288 }, { 171, 288 }, { 171, 288 },
    { 171, 288 }, { 171, 288 }, { 171, 288 }, { 171, 288 }, { 171, 288 },
};

static void flux_load_digits(flux_inst_t *f)
{
    if (!f) return;
    for (int i = 0; i < 10; i++) {
        char path[64];
        snprintf(path, sizeof(path),
                 "/spiffs/usr/share/watchfaces/flux_digit_%d.bin", i);
        f->digits[i] = app_img_load_raw(path, s_digit_dims[i].w,
                                        s_digit_dims[i].h, LV_COLOR_FORMAT_A8);
    }
}

/* One sweep animation drives BOTH panels from the same boundary value,
 * so h_top + h_bot == 502 at every frame → the panels always meet and
 * no black seam can appear between them. */
static void flux_sweep_cb(void *var, int32_t v)
{
    flux_inst_t *f = (flux_inst_t *)lv_obj_get_user_data((lv_obj_t *)var);
    if (!f) return;
    lv_coord_t y = (lv_coord_t)v;
    if (y < 0) y = 0;
    if (y > FLUX_SCREEN_H) y = FLUX_SCREEN_H;

    lv_coord_t h_top = y;                    /* top panel, from the top */
    lv_coord_t h_bot = FLUX_SCREEN_H - y;    /* bottom panel, from the bottom */

    /* 1px overlap: the bottom panel is created after (drawn on top of) the
     * top one, so let top extend 1px under bottom to hide any render seam. */
    if (h_top > 0 && h_top < FLUX_SCREEN_H) h_top++;

    if (f->panel0 && lv_obj_is_valid(f->panel0)) lv_obj_set_height(f->panel0, h_top);
    if (f->panel1 && lv_obj_is_valid(f->panel1)) lv_obj_set_height(f->panel1, h_bot);
}

/* Seconds (0..59) → panel heights (complementary, they always meet).
 *   h_bot = sec * 502/60   bottom rises from the bottom
 *   h_top = 502 - h_bot    top recedes from the top                 */
static void flux_set_seconds(flux_inst_t *f, uint8_t sec)
{
    if (!f || !f->panel0 || !lv_obj_is_valid(f->panel0)) return;
    if (sec > 59) sec = 59;

    lv_coord_t target = FLUX_SCREEN_H - (lv_coord_t)(sec * FLUX_STEP);

    if (f->step_sweep) {
        /* Preview mode: jump the boundary once per second.  No per-frame
         * animation — the full scaled face would otherwise re-render every
         * frame and tank the FPS. */
        flux_sweep_cb(f->panel0, target);
        return;
    }

    lv_anim_del(f->panel0, flux_sweep_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, f->panel0);
    lv_anim_set_exec_cb(&a, flux_sweep_cb);
    lv_anim_set_values(&a, lv_obj_get_height(f->panel0), target);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

/* Create one invisible box + a stretched digit image inside it. */
static void flux_add_digit(flux_inst_t *f, lv_obj_t *panel, const flux_digit_cfg_t *cfg,
                           lv_color_t color, lv_obj_t **img_out)
{
    lv_obj_t *box = lv_obj_create(panel);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, cfg->w);
    lv_obj_set_height(box, cfg->h);
    lv_obj_set_x(box, cfg->x);
    lv_obj_set_y(box, cfg->y);
    if (cfg->align != LV_ALIGN_DEFAULT) {
        lv_obj_set_align(box, cfg->align);
    }
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(box);
    if (f->digits[0]) {
        lv_image_set_src(img, f->digits[0]);
    } else {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_width(img, cfg->w);
    lv_obj_set_height(img, cfg->h);
    lv_obj_set_pos(img, 0, 0);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);   /* non-uniform fill */
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

    if (img_out) *img_out = img;
}

/* Build the two panels + digits into f->root using the saved colors. */
static void flux_build_panels(flux_inst_t *f)
{
    uint32_t top_rgb = g_config.flux_color_top ? g_config.flux_color_top : FLUX_COLOR_TOP;
    uint32_t bot_rgb = g_config.flux_color_bot ? g_config.flux_color_bot : FLUX_COLOR_BOT;

    /* ── panel0 : top bar (saved color), starts full screen ───────── */
    f->panel0 = lv_obj_create(f->root);
    lv_obj_set_width(f->panel0, FLUX_SCREEN_W);
    lv_obj_set_height(f->panel0, FLUX_SCREEN_H);
    lv_obj_set_align(f->panel0, LV_ALIGN_TOP_MID);
    lv_obj_remove_flag(f->panel0, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(f->panel0, lv_color_hex(top_rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f->panel0, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(f->panel0, 0, 0);
    lv_obj_set_style_radius(f->panel0, 0, 0);
    /* The digit boxes use SquareLine absolute coordinates for a 410x502
     * panel.  Without this the default 16 px padding shrinks the content
     * area to 378x470 and the aligned digit boxes land 32 px too far left,
     * overlapping the neighbouring digit. */
    lv_obj_set_style_pad_all(f->panel0, 0, 0);
    lv_obj_set_user_data(f->panel0, f);

    /* ── panel1 : bottom bar (saved color), starts 0 high ─────────── */
    f->panel1 = lv_obj_create(f->root);
    lv_obj_set_width(f->panel1, FLUX_SCREEN_W);
    lv_obj_set_height(f->panel1, 0);
    lv_obj_set_align(f->panel1, LV_ALIGN_BOTTOM_MID);
    lv_obj_remove_flag(f->panel1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(f->panel1, lv_color_hex(bot_rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f->panel1, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(f->panel1, 0, 0);
    lv_obj_set_style_radius(f->panel1, 0, 0);
    lv_obj_set_style_pad_all(f->panel1, 0, 0);   /* keep the 410x502 content area */
    lv_obj_set_user_data(f->panel1, f);

    /* ── 8 digit images (4 per panel) ─────────────────────────────── */
    for (int i = 0; i < 8; i++) {
        lv_color_t color = (i < 4) ? lv_color_white() : lv_color_black();
        lv_obj_t *panel  = (i < 4) ? f->panel0 : f->panel1;
        flux_add_digit(f, panel, &s_digit_cfg[i], color, &f->imgs[i]);
    }
}

/* watch_face_t.create — active full-screen instance. */
static lv_obj_t *flux_create(void)
{
    flux_inst_t *f = &s_flux;
    memset(f, 0, sizeof(*f));
    f->root = lv_obj_create(NULL);
    lv_obj_remove_flag(f->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(f->root, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f->root, LV_OPA_COVER, LV_PART_MAIN);
    flux_load_digits(f);
    flux_build_panels(f);
    return f->root;
}

static void flux_destroy_inst(flux_inst_t *f)
{
    if (!f) return;
    if (f->root) {
        lv_obj_delete(f->root);
        f->root = NULL;
    }
    f->panel0 = NULL;
    f->panel1 = NULL;
    for (int i = 0; i < 8; i++) f->imgs[i] = NULL;
    for (int i = 0; i < 10; i++) {
        app_img_free(f->digits[i]);
        f->digits[i] = NULL;
    }
}

static void flux_destroy(void)
{
    flux_destroy_inst(&s_flux);
}

static void flux_update_inst(flux_inst_t *f, const char *time_str, const char *date_str)
{
    if (!f) return;
    (void)date_str;

    /* Drive the bars from the live RTC seconds. */
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) == ESP_OK) {
        flux_set_seconds(f, rt.sec);
    }

    /* Parse "HH:MM" (12-hour) into 4 digits: hour tens, hour ones,
     * minute tens, minute ones.  Update both copies of each digit. */
    if (time_str && time_str[0] >= '0' && time_str[0] <= '9' &&
        time_str[1] >= '0' && time_str[1] <= '9' &&
        time_str[3] >= '0' && time_str[3] <= '9' &&
        time_str[4] >= '0' && time_str[4] <= '9') {

        uint8_t d[4] = {
            (uint8_t)(time_str[0] - '0'),   /* hour tens    → hourxx  (top-left small)   */
            (uint8_t)(time_str[1] - '0'),   /* hour ones    → hourx   (top-right big)    */
            (uint8_t)(time_str[4] - '0'),   /* minute ones  → minutex  (bottom-right big)  */
            (uint8_t)(time_str[3] - '0'),   /* minute tens  → minutexx (bottom-left small) */
        };

        for (int p = 0; p < 4; p++) {
            const lv_image_dsc_t *dsc = f->digits[d[p]];
            if (!dsc) continue;
            if (f->imgs[p]     && lv_obj_is_valid(f->imgs[p]))     lv_image_set_src(f->imgs[p], dsc);
            if (f->imgs[p + 4] && lv_obj_is_valid(f->imgs[p + 4])) lv_image_set_src(f->imgs[p + 4], dsc);
        }
    }
}

static void flux_update_time(const char *time_str, const char *date_str)
{
    flux_update_inst(&s_flux, time_str, date_str);
}

/* ── Editor preview API ────────────────────────────────────────────── */

flux_inst_t *flux_preview_create(lv_obj_t *parent)
{
    if (!parent) return NULL;
    flux_inst_t *f = (flux_inst_t *)calloc(1, sizeof(flux_inst_t));
    if (!f) return NULL;

    f->root = lv_obj_create(parent);
    lv_obj_remove_flag(f->root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(f->root, FLUX_SCREEN_W, FLUX_SCREEN_H);
    lv_obj_set_align(f->root, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(f->root, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f->root, LV_OPA_COVER, LV_PART_MAIN);
    f->step_sweep = true;   /* editor preview: stepped seconds, no sweep anim */
    flux_load_digits(f);
    flux_build_panels(f);
    return f;
}

void flux_preview_destroy(flux_inst_t *f)
{
    if (!f) return;
    flux_destroy_inst(f);
    free(f);
}

lv_obj_t *flux_preview_root(flux_inst_t *f)
{
    return f ? f->root : NULL;
}

void flux_preview_set_colors(flux_inst_t *f, uint32_t top_rgb, uint32_t bot_rgb)
{
    if (!f) return;
    if (f->panel0 && lv_obj_is_valid(f->panel0))
        lv_obj_set_style_bg_color(f->panel0, lv_color_hex(top_rgb), LV_PART_MAIN);
    if (f->panel1 && lv_obj_is_valid(f->panel1))
        lv_obj_set_style_bg_color(f->panel1, lv_color_hex(bot_rgb), LV_PART_MAIN);
}

void flux_preview_update(flux_inst_t *f, const char *time_str, const char *date_str)
{
    flux_update_inst(f, time_str, date_str);
}

const watch_face_t watch_face_flux = {
    .name           = "Flux",
    .id             = WATCH_FACE_FLUX,
    .create         = flux_create,
    .destroy        = flux_destroy,
    .update_time    = flux_update_time,
    .update_battery = NULL,   /* no battery UI on this face (yet) */
};
