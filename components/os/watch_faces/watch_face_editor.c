/**
 * @file watch_face_editor.c
 * @brief Watch-face recolour editor (Flux = 2 colour slots, Analog Hands =
 *        4 colour slots / 3 split bars).
 *
 * UI mirrors tmp/watch_face_picker/ui_watch_face_editior.c:
 *   - "COLOR" title at the top.
 *   - A green-outlined preview panel holding a LIVE scaled-down copy of the
 *     face being edited (Flux or Analog Hands).
 *   - A vertical colour strip (right side) with N sections sharing ONE
 *     centre line, separated by white split bars.  7 rainbow colours per
 *     section (N*7 swatches, N-1 splits).  Swatch size scales by distance to
 *     the centre (coverflow) and the strip snaps to the centre.
 *   - The saved colours (one per section) keep a green outline.  Scrolling
 *     previews the centred colour on its slot (live, not saved); pressing
 *     the centred swatch saves it.
 *   - A green chip (bottom-left) shows the centred colour's name.
 *   - GPIO0 exits: the preview scales to full screen and the edited face is
 *     re-created with the saved colours.
 *
 * Entry/exit transitions (same as the Flux editor):
 *   - Open: the picker's centred card preview scales + slides into the
 *     editor preview panel (lv_layer_top overlay, move+scale anim).
 *   - Exit: a snapshot of the live preview scales up to full screen.
 *
 * Each editable face contributes an `editor_face_t` (colour slots, live
 * preview callbacks) — see watch_face_editor_open()/watch_face_editor_supported().
 */
#include "watch_face_editor.h"
#include "watch_face.h"
#include "watch_face_flux.h"
#include "watch_face_hands.h"
#include "watch_face_picker.h"
#include "app_events.h"
#include "app_ui.h"
#include "app_config.h"
#include "hw_rtc.h"
#include "fonts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp/display.h"
#include "draw/snapshot/lv_snapshot.h"
#include "draw/lv_draw_buf.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "WF_EDIT";

#define SCR_W    BSP_LCD_H_RES          /* 410 */
#define SCR_H    BSP_LCD_V_RES          /* 502 */

/* ── Palette ───────────────────────────────────────────────────────── */
#define COLOR_BG      lv_color_hex(0x000000)
#define COLOR_TITLE   lv_color_hex(0xFFFFFF)
#define COLOR_ACCENT  lv_color_hex(0x41CC75)   /* green selection / target outline */
#define COLOR_SPLIT   lv_color_hex(0xFFFFFF)
#define COLOR_CHIP_BG COLOR_ACCENT
#define COLOR_CHIP_TX lv_color_hex(0x000000)

/* 11 colours per section (all sections share this palette): 7 rainbow + 4
 * neutrals.  The first 7 keep the original Flux order; the neutrals let the
 * Analog Hands defaults (black bg, silver hands) be re-selected. */
#define PER_SECTION   11
#define MAX_SLOTS     4          /* max colour slots (Hands = 4) */
typedef struct {
    uint32_t    rgb;
    const char *name;
} editor_color_t;
static const editor_color_t s_palette[PER_SECTION] = {
    { 0xFF0000, "RED"     },
    { 0xFF8800, "ORANGE"  },
    { 0xFF4800, "AMBER"   },   /* Flux default top */
    { 0xE1FF00, "YELLOW"  },   /* Flux default bottom */
    { 0x00E676, "GREEN"   },
    { 0x2196F3, "BLUE"    },
    { 0x9C27B0, "PURPLE"  },
    { 0x000000, "BLACK"   },
    { 0xFFFFFF, "WHITE"   },
    { 0xE0E0E0, "SILVER"  },   /* Analog Hands default hands */
    { 0x8A8A8A, "GREY"    },
};

/* ── Layout (matches the SquareLine editor) ────────────────────────── */
#define TITLE_Y         20
#define TARGET_W        321
#define TARGET_H        381
#define TARGET_X        -32
#define TARGET_Y        8
#define TARGET_BORDER   3
#define TARGET_R        70
#define TARGET_INNER_PCT 95
#define TARGET_INNER_R  60
#define CHIP_X          125
#define CHIP_Y          -9
#define CHIP_H          38
#define CHIP_R          8
#define CHIP_PAD_X      18
#define STRIP_W         52
#define STRIP_H         428
#define STRIP_X         -15
#define STRIP_Y         15

/* Colour strip (one centre, snap) */
#define SWATCH_MAX      30      /* centred swatch */
#define SWATCH_PITCH    34      /* snap step (must be >= SWATCH_MAX) */
#define OUTLINE_W       3

/* ── Editable-face descriptor ──────────────────────────────────────── */

/* Preview instances are opaque to the editor; each face drives them. */
typedef struct editor_face {
    const char *name;
    uint8_t     face_id;
    uint8_t     n_slots;                          /* colour slots (2/4) */
    void      *(*preview_create)(lv_obj_t *parent);
    void       (*preview_destroy)(void *inst);
    lv_obj_t  *(*preview_root)(void *inst);
    void       (*preview_apply)(void *inst, const uint32_t *slot_rgb);
    void       (*preview_tick)(void *inst, const char *time_str,
                               const char *date_str);
    uint32_t   (*slot_get)(uint8_t slot);
    void       (*slot_set)(uint8_t slot, uint32_t rgb);
} editor_face_t;

/* ── Flux adapter (2 slots: top panel, bottom panel) ─────────────── */
static void *flux_pv_create(lv_obj_t *p)      { return flux_preview_create(p); }
static void  flux_pv_destroy(void *v)         { flux_preview_destroy((flux_inst_t *)v); }
static lv_obj_t *flux_pv_root(void *v)        { return flux_preview_root((flux_inst_t *)v); }
static void  flux_pv_apply(void *v, const uint32_t *rgb)
{
    flux_preview_set_colors((flux_inst_t *)v, rgb[0], rgb[1]);
}
static void flux_pv_tick(void *v, const char *t, const char *d)
{
    flux_preview_update((flux_inst_t *)v, t, d);
}
static uint32_t flux_slot_get(uint8_t slot)
{
    return (slot == 0) ? g_config.flux_color_top : g_config.flux_color_bot;
}
static void flux_slot_set(uint8_t slot, uint32_t rgb)
{
    if (slot == 0) g_config.flux_color_top = rgb;
    else           g_config.flux_color_bot = rgb;
}

/* ── Analog Hands adapter (4 slots: bg+pin, hour, minute, second) ─── */
static void *hands_pv_create(lv_obj_t *p)     { return hands_preview_create(p); }
static void  hands_pv_destroy(void *v)        { hands_preview_destroy((hands_inst_t *)v); }
static lv_obj_t *hands_pv_root(void *v)       { return hands_preview_root((hands_inst_t *)v); }
static void  hands_pv_apply(void *v, const uint32_t *rgb)
{
    hands_preview_set_colors((hands_inst_t *)v,
                             rgb[0], rgb[1], rgb[2], rgb[3]);
}
static void hands_pv_tick(void *v, const char *t, const char *d)
{
    hands_preview_update((hands_inst_t *)v, t, d);
}
static uint32_t hands_slot_get(uint8_t slot)
{
    switch (slot) {
        case 0:  return g_config.hands_face_bg;
        case 1:  return g_config.hands_face_hour;
        case 2:  return g_config.hands_face_min;
        default: return g_config.hands_face_sec;
    }
}
static void hands_slot_set(uint8_t slot, uint32_t rgb)
{
    switch (slot) {
        case 0:  g_config.hands_face_bg   = rgb; break;
        case 1:  g_config.hands_face_hour = rgb; break;
        case 2:  g_config.hands_face_min  = rgb; break;
        default: g_config.hands_face_sec  = rgb; break;
    }
}

static const editor_face_t s_faces[] = {
    {
        .name = "Flux", .face_id = WATCH_FACE_FLUX, .n_slots = 2,
        .preview_create = flux_pv_create, .preview_destroy = flux_pv_destroy,
        .preview_root = flux_pv_root, .preview_apply = flux_pv_apply,
        .preview_tick = flux_pv_tick, .slot_get = flux_slot_get,
        .slot_set = flux_slot_set,
    },
    {
        .name = "Analog Hands", .face_id = WATCH_FACE_HANDS, .n_slots = 4,
        .preview_create = hands_pv_create, .preview_destroy = hands_pv_destroy,
        .preview_root = hands_pv_root, .preview_apply = hands_pv_apply,
        .preview_tick = hands_pv_tick, .slot_get = hands_slot_get,
        .slot_set = hands_slot_set,
    },
};

static const editor_face_t *face_for_id(uint8_t id)
{
    for (uint32_t i = 0; i < sizeof(s_faces) / sizeof(s_faces[0]); i++) {
        if (s_faces[i].face_id == id) return &s_faces[i];
    }
    return NULL;
}

/* ── State ─────────────────────────────────────────────────────────── */
typedef struct {
    bool        active;
    const editor_face_t *face;      /* face being edited */
    uint8_t     n_slots;
    lv_obj_t   *scr;
    lv_obj_t   *preview_cont, *preview_panel, *preview_clip;
    lv_obj_t   *chip, *chip_label;
    lv_obj_t   *strip;
    lv_obj_t   *swatches[MAX_SLOTS][PER_SECTION];
    void       *preview;            /* live preview instance (opaque) */
    uint8_t     saved[MAX_SLOTS];   /* palette index per section (green outline) */
    uint8_t     centered;           /* strip index at the centre line */
    uint8_t     last_sec;           /* last live-tick second (smooth sweep) */
    int64_t     last_tick_us;

    /* Enter/exit transition overlay */
    lv_obj_t    *trans_img;
    uint8_t     *trans_snap;    /* PSRAM copy used as the overlay source */
    lv_img_dsc_t trans_dsc;
} editor_ctx_t;

static editor_ctx_t s_ed;

/* ── Strip geometry (N sections, PER_SECTION swatches, N-1 splits) ── */

/* Section k's first strip index.  Each section occupies PER_SECTION
 * swatches; a split bar follows every section except the last. */
static int section_start(int k)        { return k * PER_SECTION + k; }
static int split_after(int k)          { return (k + 1) * PER_SECTION + k; }
static int strip_total(uint8_t n)      { return (int)n * PER_SECTION + (n - 1); }
static bool is_split_index(int si)
{
    for (int k = 0; k < s_ed.n_slots - 1; k++) {
        if (si == split_after(k)) return true;
    }
    return false;
}

/* Map a strip index (swatch, not a split) to (section, palette index). */
static void strip_section(int si, int *sec, int *pi)
{
    for (int k = 0; k < s_ed.n_slots; k++) {
        int start = section_start(k);
        if (si >= start && si < start + PER_SECTION) {
            *sec = k;
            *pi  = si - start;
            return;
        }
    }
    *sec = 0;
    *pi  = 0;
}

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Palette index of a 0xRRGGBB colour (nearest match), 0 if absent. */
static uint8_t find_palette_index(uint32_t rgb)
{
    int best = 0, best_d = INT32_MAX;
    for (int i = 0; i < PER_SECTION; i++) {
        int dr = (int)((s_palette[i].rgb >> 16) & 0xFF) - (int)((rgb >> 16) & 0xFF);
        int dg = (int)((s_palette[i].rgb >> 8)  & 0xFF) - (int)((rgb >> 8)  & 0xFF);
        int db = (int)(s_palette[i].rgb & 0xFF)         - (int)(rgb & 0xFF);
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

static void trans_cleanup(void)
{
    if (s_ed.trans_img) { lv_obj_delete(s_ed.trans_img); s_ed.trans_img = NULL; }
    if (s_ed.trans_snap) { free(s_ed.trans_snap); s_ed.trans_snap = NULL; }
    memset(&s_ed.trans_dsc, 0, sizeof(s_ed.trans_dsc));
}

/* ── Live preview ──────────────────────────────────────────────────── */

/* Recolour the live preview: saved colours for every slot, but the slot of
 * the centred swatch previews the palette colour under the centre. */
static void preview_recolor(void)
{
    if (!s_ed.preview || !s_ed.face) return;
    int sec, pi;
    strip_section(s_ed.centered, &sec, &pi);
    uint32_t cur[MAX_SLOTS];
    for (int k = 0; k < s_ed.n_slots; k++) cur[k] = s_ed.face->slot_get(k);
    cur[sec] = s_palette[pi].rgb;          /* centred colour previews live */
    s_ed.face->preview_apply(s_ed.preview, cur);
}

static void chip_update(void)
{
    int sec, pi;
    strip_section(s_ed.centered, &sec, &pi);
    (void)sec;
    if (s_ed.chip_label) lv_label_set_text(s_ed.chip_label, s_palette[pi].name);
    if (s_ed.chip) lv_obj_set_width(s_ed.chip, LV_SIZE_CONTENT);  /* auto-size to name */
}

static void outlines_update(void)
{
    /* Centred swatch = WHITE outline (the colour about to snap / be
     * saved); saved swatches = green outline.  Centred wins if the same
     * swatch is both (it is the current preview). */
    int csec = 0, cpi = 0;
    strip_section(s_ed.centered, &csec, &cpi);
    for (int sec = 0; sec < s_ed.n_slots; sec++) {
        for (int pi = 0; pi < PER_SECTION; pi++) {
            lv_obj_t *sw = s_ed.swatches[sec][pi];
            if (!sw) continue;
            bool is_centered = (sec == csec && pi == cpi);
            bool is_saved    = (pi == s_ed.saved[sec]);
            if (is_centered) {
                lv_obj_set_style_outline_color(sw, lv_color_hex(0xFFFFFF), 0);
                lv_obj_set_style_outline_opa(sw, LV_OPA_COVER, 0);
            } else if (is_saved) {
                lv_obj_set_style_outline_color(sw, COLOR_ACCENT, 0);
                lv_obj_set_style_outline_opa(sw, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_outline_opa(sw, LV_OPA_TRANSP, 0);
            }
        }
    }
}

/* Coverflow sizing ONLY — runs on every scroll frame (cheap: just resize
 * the small swatches, never touch the live preview).  The split bars share
 * the same spacing but are never scaled; swatches grow to SWATCH_MAX at
 * the centre and shrink to ~0 at the two ends. */
static void strip_coverflow(void)
{
    if (!s_ed.strip) return;
    int32_t scroll_y = lv_obj_get_scroll_y(s_ed.strip);
    int32_t total = strip_total(s_ed.n_slots);
    const int32_t range = STRIP_H / 2;
    for (int i = 0; i < total; i++) {
        if (is_split_index(i)) continue;   /* split bar: fixed size */
        int32_t dist = (int32_t)i * SWATCH_PITCH - scroll_y;
        if (dist < 0) dist = -dist;
        int32_t size = SWATCH_MAX * (range - dist) / range;
        if (size < 4) size = 4;           /* faint dot at the extremes */
        if (size > SWATCH_MAX) size = SWATCH_MAX;
        int sec, pi;
        strip_section(i, &sec, &pi);
        lv_obj_t *sw = s_ed.swatches[sec][pi];
        if (!sw) continue;
        if (lv_obj_get_width(sw) != size) {
            lv_obj_set_width(sw, size);
            lv_obj_set_height(sw, size);
            lv_obj_set_x(sw, (STRIP_W - size) / 2);
        }
    }
}

/* Snap-settle: recompute the centred index and re-render the LIVE preview
 * + chip.  Runs ONLY on LV_EVENT_SCROLL_END (and once at build), NOT on
 * every scroll frame — re-rendering the transform-scaled face preview is
 * expensive, so deferring it to the snap keeps scrolling smooth. */
static void strip_apply(void)
{
    if (!s_ed.strip) return;
    int32_t scroll_y = lv_obj_get_scroll_y(s_ed.strip);
    int32_t total = strip_total(s_ed.n_slots);

    /* Centred strip index; splits are never centred. */
    int32_t si = (scroll_y + SWATCH_PITCH / 2) / SWATCH_PITCH;
    if (si < 0) si = 0;
    if (si > total - 1) si = total - 1;
    if (is_split_index((int)si)) {
        si = (scroll_y < (int32_t)si * SWATCH_PITCH) ? si - 1 : si + 1;
    }
    if (si < 0) si = 0;
    if (si > total - 1) si = total - 1;

    bool centered_changed = ((uint8_t)si != s_ed.centered);
    s_ed.centered = (uint8_t)si;

    strip_coverflow();

    if (centered_changed) {
        preview_recolor();
        outlines_update();               /* white outline follows the snap */
    }
    chip_update();                       /* cheap; also syncs on first build */
    g_last_activity_us = esp_timer_get_time();
}

static void strip_scroll_cb(lv_event_t *e)
{
    strip_coverflow();
}

static void strip_scroll_end_cb(lv_event_t *e)
{
    strip_apply();
}

/* ── Colour selection ──────────────────────────────────────────────── */

static void swatch_click_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int si = -1;
    for (int sec = 0; sec < s_ed.n_slots && si < 0; sec++)
        for (int pi = 0; pi < PER_SECTION; pi++)
            if (s_ed.swatches[sec][pi] == sw) {
                si = section_start(sec) + pi;   /* strip index */
                break;
            }
    if (si < 0) return;

    if (si == s_ed.centered) {
        /* Press the centred swatch → save it for its section.  MUST
         * update g_config BEFORE app_config_save(), otherwise the OLD
         * colours get written to SPIFFS and exit re-creates the face stale. */
        int sec, pi;
        strip_section(si, &sec, &pi);
        s_ed.saved[sec] = (uint8_t)pi;
        s_ed.face->slot_set((uint8_t)sec, s_palette[pi].rgb);
        app_config_save();
        outlines_update();
        preview_recolor();
        app_events_request_vibe_click();
        ESP_LOGI(TAG, "%s: saved section %d colour = %s (0x%06X)",
                 s_ed.face->name, sec, s_palette[pi].name, s_palette[pi].rgb);
    } else {
        /* Tap a non-centred swatch → scroll it to the centre (preview only). */
        lv_obj_scroll_to_y(s_ed.strip, (int32_t)si * SWATCH_PITCH, LV_ANIM_ON);
    }
}

/* ── Move + scale transition helper ────────────────────────────────── */
typedef struct {
    lv_obj_t  *img;
    lv_coord_t x0, y0, x1, y1;
    int32_t    s0, s1;
} move_scale_t;

static void move_scale_exec(void *var, int32_t v)
{
    move_scale_t *d = (move_scale_t *)var;
    lv_obj_set_pos(d->img,
                   d->x0 + (d->x1 - d->x0) * v / 1000,
                   d->y0 + (d->y1 - d->y0) * v / 1000);
    lv_image_set_scale(d->img, (uint32_t)(d->s0 + (d->s1 - d->s0) * v / 1000));
}

static void move_scale_ready(lv_anim_t *a)
{
    free(lv_anim_get_user_data(a));   /* the move_scale_t data */
    trans_cleanup();
}

/* ── Screen build ──────────────────────────────────────────────────── */

static void build_editor_screen(void)
{
    s_ed.scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_ed.scr);
    lv_obj_set_style_bg_color(s_ed.scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_ed.scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_ed.scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(s_ed.scr);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    lv_label_set_text(title, "COLOR");
    lv_obj_set_style_text_color(title, COLOR_TITLE, 0);
    lv_obj_set_style_text_font(title, &noto_font_24, 0);

    /* Preview target: green-outlined container */
    s_ed.preview_cont = lv_obj_create(s_ed.scr);
    lv_obj_remove_style_all(s_ed.preview_cont);
    lv_obj_set_size(s_ed.preview_cont, TARGET_W, TARGET_H);
    lv_obj_align(s_ed.preview_cont, LV_ALIGN_CENTER, TARGET_X, TARGET_Y);
    lv_obj_remove_flag(s_ed.preview_cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_ed.preview_panel = lv_obj_create(s_ed.preview_cont);
    lv_obj_set_size(s_ed.preview_panel, lv_pct(100), lv_pct(100));
    lv_obj_set_align(s_ed.preview_panel, LV_ALIGN_CENTER);
    lv_obj_remove_flag(s_ed.preview_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_ed.preview_panel, TARGET_R, 0);
    lv_obj_set_style_bg_color(s_ed.preview_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_ed.preview_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ed.preview_panel, COLOR_ACCENT, 0);
    lv_obj_set_style_border_opa(s_ed.preview_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ed.preview_panel, TARGET_BORDER, 0);
    lv_obj_set_style_clip_corner(s_ed.preview_panel, true, 0);

    s_ed.preview_clip = lv_obj_create(s_ed.preview_panel);
    lv_obj_remove_style_all(s_ed.preview_clip);
    lv_obj_set_size(s_ed.preview_clip, lv_pct(TARGET_INNER_PCT), lv_pct(TARGET_INNER_PCT));
    lv_obj_set_align(s_ed.preview_clip, LV_ALIGN_CENTER);
    lv_obj_remove_flag(s_ed.preview_clip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_ed.preview_clip, TARGET_INNER_R, 0);
    lv_obj_set_style_clip_corner(s_ed.preview_clip, true, 0);

    /* Live face preview scaled to fit the clip */
    if (s_ed.face) {
        s_ed.preview = s_ed.face->preview_create(s_ed.preview_clip);
        if (s_ed.preview) {
            lv_obj_t *root = s_ed.face->preview_root(s_ed.preview);
            lv_obj_update_layout(s_ed.scr);
            lv_coord_t cw = lv_obj_get_width(s_ed.preview_clip);
            lv_coord_t ch = lv_obj_get_height(s_ed.preview_clip);
            int32_t scale = (cw * 256 / 410 < ch * 256 / 502) ? cw * 256 / 410 : ch * 256 / 502;
            if (scale < 64) scale = 64;
            /* Scale about the root's CENTRE — the default transform pivot is
             * the top-left corner, which pushes the scaled face off-centre. */
            lv_obj_set_style_transform_pivot_x(root, 410 / 2, 0);
            lv_obj_set_style_transform_pivot_y(root, 502 / 2, 0);
            lv_obj_set_style_transform_scale_x(root, scale, 0);
            lv_obj_set_style_transform_scale_y(root, scale, 0);
        }
    }

    /* Green chip (auto-width) showing the centred colour name */
    s_ed.chip = lv_obj_create(s_ed.scr);
    lv_obj_remove_style_all(s_ed.chip);
    lv_obj_set_height(s_ed.chip, CHIP_H);
    lv_obj_set_width(s_ed.chip, LV_SIZE_CONTENT);
    lv_obj_align(s_ed.chip, LV_ALIGN_BOTTOM_LEFT, CHIP_X, CHIP_Y);
    lv_obj_set_style_radius(s_ed.chip, CHIP_R, 0);
    lv_obj_set_style_bg_color(s_ed.chip, COLOR_CHIP_BG, 0);
    lv_obj_set_style_bg_opa(s_ed.chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_ed.chip, CHIP_PAD_X, 0);
    lv_obj_set_style_pad_right(s_ed.chip, CHIP_PAD_X, 0);
    lv_obj_remove_flag(s_ed.chip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_ed.chip_label = lv_label_create(s_ed.chip);
    lv_obj_center(s_ed.chip_label);
    lv_obj_set_style_text_color(s_ed.chip_label, COLOR_CHIP_TX, 0);
    lv_obj_set_style_text_font(s_ed.chip_label, &noto_font_24, 0);

    /* Colour strip (vertical, one centre, snap) */
    s_ed.strip = lv_obj_create(s_ed.scr);
    lv_obj_remove_style_all(s_ed.strip);
    lv_obj_set_size(s_ed.strip, STRIP_W, STRIP_H);
    lv_obj_align(s_ed.strip, LV_ALIGN_RIGHT_MID, STRIP_X, STRIP_Y);
    lv_obj_set_style_bg_opa(s_ed.strip, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(s_ed.strip, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_ed.strip, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_ed.strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_top(s_ed.strip, (STRIP_H - SWATCH_PITCH) / 2, 0);
    lv_obj_set_style_pad_bottom(s_ed.strip, (STRIP_H - SWATCH_PITCH) / 2, 0);
    lv_obj_add_event_cb(s_ed.strip, strip_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_ed.strip, strip_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    /* N*7 swatches + (N-1) splits at uniform pitch.  Splits share the same
     * spacing but are NOT snappable/fixed. */
    int total_items = strip_total(s_ed.n_slots);
    for (int i = 0; i < total_items; i++) {
        if (is_split_index(i)) {
            lv_obj_t *split = lv_obj_create(s_ed.strip);
            lv_obj_remove_style_all(split);
            lv_obj_set_width(split, 24);
            lv_obj_set_height(split, 6);
            lv_obj_set_x(split, (STRIP_W - 24) / 2);
            lv_obj_set_y(split, i * SWATCH_PITCH);
            lv_obj_set_style_radius(split, 5, 0);
            lv_obj_set_style_bg_color(split, COLOR_SPLIT, 0);
            lv_obj_set_style_bg_opa(split, LV_OPA_COVER, 0);
            lv_obj_remove_flag(split, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(split, LV_OBJ_FLAG_SNAPPABLE);
            continue;
        }
        int sec, pi;
        strip_section(i, &sec, &pi);
        lv_obj_t *sw = lv_obj_create(s_ed.strip);
        lv_obj_remove_style_all(sw);
        lv_obj_set_width(sw, SWATCH_MAX);
        lv_obj_set_height(sw, SWATCH_MAX);
        lv_obj_set_x(sw, (STRIP_W - SWATCH_MAX) / 2);
        lv_obj_set_y(sw, i * SWATCH_PITCH);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(s_palette[pi].rgb), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_color(sw, COLOR_ACCENT, 0);
        lv_obj_set_style_outline_width(sw, OUTLINE_W, 0);
        lv_obj_set_style_outline_pad(sw, 2, 0);
        lv_obj_set_style_outline_opa(sw, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);   /* SNAPPABLE is default */
        lv_obj_add_event_cb(sw, swatch_click_cb, LV_EVENT_CLICKED, NULL);
        s_ed.swatches[sec][pi] = sw;
    }

    /* Saved colours from config; centre on section 0's saved colour */
    for (int k = 0; k < s_ed.n_slots; k++) {
        s_ed.saved[k] = find_palette_index(s_ed.face->slot_get((uint8_t)k));
    }
    s_ed.centered = s_ed.saved[0];
    lv_obj_scroll_to_y(s_ed.strip, (int32_t)s_ed.saved[0] * SWATCH_PITCH, LV_ANIM_OFF);
    outlines_update();
    strip_apply();
}

static void destroy_editor(void)
{
    if (s_ed.preview && s_ed.face) {
        s_ed.face->preview_destroy(s_ed.preview);
        s_ed.preview = NULL;
    }
    if (s_ed.scr) {
        lv_obj_delete(s_ed.scr);
        s_ed.scr = NULL;
    }
    s_ed.preview_cont = NULL;
    s_ed.preview_panel = NULL;
    s_ed.preview_clip = NULL;
    s_ed.chip = NULL;
    s_ed.chip_label = NULL;
    s_ed.strip = NULL;
    memset(s_ed.swatches, 0, sizeof(s_ed.swatches));
}

/* ── Public API ────────────────────────────────────────────────────── */

bool watch_face_editor_supported(uint8_t face_id)
{
    return face_for_id(face_id) != NULL;
}

void watch_face_editor_open(uint8_t face_id)
{
    if (s_ed.active) return;
    const editor_face_t *face = face_for_id(face_id);
    if (!face) {
        ESP_LOGI(TAG, "face %u has no colour editor", face_id);
        return;
    }

    /* Capture the picker's centred preview (copy so the picker can be
     * closed safely during the enter transition). */
    const lv_img_dsc_t *src = watch_face_picker_preview_dsc();
    uint8_t *src_copy = NULL;
    lv_img_dsc_t src_dsc;
    memset(&src_dsc, 0, sizeof(src_dsc));
    lv_area_t src_rect;
    watch_face_picker_preview_rect(&src_rect);
    if (src && src->data && src->data_size > 0) {
        src_copy = heap_caps_malloc(src->data_size, MALLOC_CAP_SPIRAM);
        if (src_copy) {
            memcpy(src_copy, src->data, src->data_size);
            src_dsc = *src;
            src_dsc.data = src_copy;
        }
    }

    /* Build + load the editor screen. */
    memset(&s_ed, 0, sizeof(s_ed));
    s_ed.face = face;
    s_ed.n_slots = face->n_slots;
    build_editor_screen();
    lv_screen_load(s_ed.scr);
    s_ed.active = true;
    g_current_screen = APP_SCREEN_WATCH_FACE_EDITOR;
    app_events_request_vibe_click();
    ESP_LOGI(TAG, "editor opened (%s, %d colour slots)", face->name,
             face->n_slots);

    /* Close the picker (it launched us; we own the flow now). */
    watch_face_picker_force_close();

    /* Enter transition: picker preview scales + slides into the editor
     * preview panel. */
    if (src_copy) {
        lv_area_t tgt;
        lv_obj_update_layout(s_ed.scr);
        lv_obj_get_coords(s_ed.preview_clip, &tgt);

        s_ed.trans_snap = src_copy;
        s_ed.trans_dsc = src_dsc;
        lv_coord_t w0 = src_dsc.header.w, h0 = src_dsc.header.h;
        lv_coord_t tw = tgt.x2 - tgt.x1 + 1, th = tgt.y2 - tgt.y1 + 1;

        lv_obj_t *img = lv_image_create(lv_layer_top());
        lv_obj_remove_style_all(img);
        lv_obj_set_size(img, w0, h0);
        lv_obj_set_pos(img, src_rect.x1, src_rect.y1);
        lv_image_set_src(img, &s_ed.trans_dsc);
        lv_image_set_pivot(img, w0 / 2, h0 / 2);
        lv_image_set_scale(img, LV_SCALE_NONE);
        s_ed.trans_img = img;

        /* Final object top-left so the scaled image lands on the target
         * rect (scale is about the object centre). */
        lv_coord_t x1 = tgt.x1 + (tw - w0) / 2;
        lv_coord_t y1 = tgt.y1 + (th - h0) / 2;
        int32_t s1 = tw * LV_SCALE_NONE / w0;

        move_scale_t *d = (move_scale_t *)malloc(sizeof(move_scale_t));
        if (d) {
            d->img = img; d->x0 = src_rect.x1; d->y0 = src_rect.y1;
            d->x1 = x1; d->y1 = y1; d->s0 = LV_SCALE_NONE; d->s1 = s1;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, d);
            lv_anim_set_exec_cb(&a, move_scale_exec);
            lv_anim_set_values(&a, 0, 1000);
            lv_anim_set_time(&a, 300);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_ready_cb(&a, move_scale_ready);
            lv_anim_set_user_data(&a, d);
            lv_anim_start(&a);
        } else {
            trans_cleanup();
        }
    }
}

void watch_face_editor_process(void)
{
    if (!s_ed.active || !s_ed.preview || !s_ed.face) return;

    /* Build a plausible local time string (same as app_ui). */
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return;

    /* Only tick once per second boundary so the sweep restarts smoothly. */
    if (rt.sec == s_ed.last_sec && s_ed.last_tick_us != 0) return;
    s_ed.last_sec = rt.sec;
    s_ed.last_tick_us = esp_timer_get_time();

    time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
    hw_rtc_time_t local;
    hw_rtc_from_unix(ts, &local);
    int h12 = local.hour % 12; if (h12 == 0) h12 = 12;
    char time_buf[16], date_buf[32];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", h12, local.min);
    snprintf(date_buf, sizeof(date_buf), "Edit");
    s_ed.face->preview_tick(s_ed.preview, time_buf, date_buf);
}

void watch_face_editor_exit(void)
{
    if (!s_ed.active) return;
    s_ed.active = false;

    /* Snapshot the live preview and scale it to full screen. */
    lv_area_t clip;
    lv_obj_update_layout(s_ed.scr);
    lv_obj_get_coords(s_ed.preview_clip, &clip);
    lv_draw_buf_t *snap = lv_snapshot_take(s_ed.preview_clip, LV_COLOR_FORMAT_RGB565);
    if (snap) {
        s_ed.trans_snap = heap_caps_malloc(snap->data_size, MALLOC_CAP_SPIRAM);
        if (s_ed.trans_snap) {
            memcpy(s_ed.trans_snap, snap->data, snap->data_size);
            memset(&s_ed.trans_dsc, 0, sizeof(s_ed.trans_dsc));
            s_ed.trans_dsc.header.w = snap->header.w;
            s_ed.trans_dsc.header.h = snap->header.h;
            s_ed.trans_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            s_ed.trans_dsc.data_size = snap->data_size;
            s_ed.trans_dsc.data = s_ed.trans_snap;

            lv_coord_t w0 = snap->header.w, h0 = snap->header.h;
            lv_obj_t *img = lv_image_create(lv_layer_top());
            lv_obj_remove_style_all(img);
            lv_obj_set_size(img, w0, h0);
            lv_obj_set_pos(img, clip.x1, clip.y1);
            lv_image_set_src(img, &s_ed.trans_dsc);
            lv_image_set_pivot(img, w0 / 2, h0 / 2);
            lv_image_set_scale(img, LV_SCALE_NONE);
            s_ed.trans_img = img;

            lv_coord_t x1 = (SCR_W - w0) / 2;
            lv_coord_t y1 = (SCR_H - h0) / 2;
            int32_t s1 = SCR_W * LV_SCALE_NONE / w0;
            move_scale_t *d = (move_scale_t *)malloc(sizeof(move_scale_t));
            if (d) {
                d->img = img; d->x0 = clip.x1; d->y0 = clip.y1;
                d->x1 = x1; d->y1 = y1; d->s0 = LV_SCALE_NONE; d->s1 = s1;
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, d);
                lv_anim_set_exec_cb(&a, move_scale_exec);
                lv_anim_set_values(&a, 0, 1000);
                lv_anim_set_time(&a, 300);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                lv_anim_set_ready_cb(&a, move_scale_ready);
                lv_anim_set_user_data(&a, d);
                lv_anim_start(&a);
            }
        }
        lv_draw_buf_destroy(snap);
    }

    uint8_t face_id = s_ed.face ? s_ed.face->face_id : WATCH_FACE_FLUX;
    const char *face_name = s_ed.face ? s_ed.face->name : "?";

    /* Tear down the editor + preview (colours already saved on press). */
    destroy_editor();

    /* Re-create the edited face full-screen with the saved colours. */
    bool ok = watch_face_activate(face_id);
    g_current_screen = APP_SCREEN_WATCH_FACE;
    if (ok) {
        app_ui_update_time();
        app_ui_update_battery();
        app_events_request_vibe_click();
    }
    g_last_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "editor exited — %s recoloured", face_name);
}

bool watch_face_editor_is_active(void)
{
    return s_ed.active;
}
