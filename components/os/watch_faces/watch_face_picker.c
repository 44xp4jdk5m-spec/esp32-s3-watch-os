/**
 * @file watch_face_picker.c
 * @brief Apple-Watch-style watch face picker (SquareLine-adopted UI).
 *
 * The visuals mirror tmp/watch_face_picker/ui_watch_face_picker.c:
 *   - Solid black background.
 *   - A horizontal snap-scroll carousel of FIXED-size (281x326) cards:
 *       container → outline panel (5px gray border, radius 70) → inner
 *       rounded clip (radius 55) → scaled preview image.
 *   - Each card has its own face-name label above it.
 *   - Off-center cards are dimmed (container opa 100) per the design.
 *   - A gray "Edit" pill (180x70) sits at the bottom (placeholder).
 *   - Tap the centered card to select it; tap an off-center card to
 *     center it first; GPIO0 cancels.
 *   - Scale transitions: entering the picker shrinks the current face
 *     into its preview; selecting expands the preview to full screen.
 *
 * Previews are generated ASYNCHRONOUSLY: the UI (cards, names, spinners)
 * is built instantly, then one thumbnail per face is generated per
 * main-loop iteration (current face first, then neighbours outward) so
 * opening the picker never blocks on SPIFFS decoding.  No per-face
 * preview code is needed.
 */
#include "watch_face_picker.h"
#include "watch_face_editor.h"
#include "watch_face.h"
#include "app_events.h"
#include "app_ui.h"
#include "app_config.h"
#include "app_notification_center.h"
#include "hw_touch.h"
#include "ui.h"
#include "spinner.h"
#include "fonts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp/display.h"
#include "draw/snapshot/lv_snapshot.h"
#include "draw/lv_draw_buf.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WF_PICKER";

#define SCR_W    BSP_LCD_H_RES          /* 410 */
#define SCR_H    BSP_LCD_V_RES          /* 502 */

/* ── Palette (colors, organized) ──────────────────────────────────── */
#define COLOR_BG          lv_color_hex(0x000000)  /* screen / panel background */
#define COLOR_OUTLINE     lv_color_hex(0x515151)  /* card border + edit button */
#define COLOR_TEXT        lv_color_hex(0xFFFFFF)  /* names + edit label */
#define OPA_SIDE_CARD     100                     /* dim for off-center cards */

/* ── Card layout (adopted from tmp/watch_face_picker SquareLine) ──── */
#define CARD_W      281
#define CARD_H      326
#define PANEL_R     70                  /* outline panel corner radius */
#define CARD_BORDER 5                   /* outline width (SquareLine) */
#define INNER_PCT   90                  /* inner clip = 90% of panel content */
#define INNER_R     55                  /* inner clip corner radius */
#define GAP         7                   /* STEP = 288 (matches ±288 spacing) */
#define STEP        (CARD_W + GAP)

#define NAME_Y      35                  /* name labels above the cards */
#define EDIT_BTN_W  180
#define EDIT_BTN_H  70
#define EDIT_BTN_R  50
#define EDIT_BTN_OFF -1                 /* bottom offset (design uses -1) */

/* ── Gesture params ────────────────────────────────────────────────── */
#define LONG_PRESS_MS   450             /* hold this long to open the picker */
#define SWIPE_EDGE      20              /* top zone reserved for NC swipe */

/* Inner preview clip size (90% of the panel's content area). */
#define INNER_W   ((CARD_W - 2 * CARD_BORDER) * INNER_PCT / 100)   /* 243 */
#define INNER_H   ((CARD_H - 2 * CARD_BORDER) * INNER_PCT / 100)   /* 284 */

/* ── Per-face picker item ──────────────────────────────────────────── */
typedef struct {
    const watch_face_t *face;
    lv_obj_t *card;      /* container 281x326 (moves + dims in the carousel) */
    lv_obj_t *panel;     /* outline panel (border, radius, black bg) */
    lv_obj_t *clip;      /* inner rounded clip for the preview */
    lv_obj_t *img;       /* preview image (fills the clip) */
    lv_obj_t *name;      /* face name label above the card */
    lv_obj_t *spinner;   /* system hexagon spinner until thumb is ready */
    lv_img_dsc_t dsc;    /* thumbnail descriptor (single size) */
    uint16_t *data;      /* PSRAM thumbnail buffer */
} picker_item_t;

static bool s_open       = false;
static picker_item_t s_items[WATCH_FACE_MAX];
static uint8_t s_count   = 0;
static uint8_t s_centered = 0;

/* Async preview queue: face indices ordered by distance from the current
 * face (current first, then neighbours outward).  The picker UI is built
 * instantly; thumbnails are generated one per main-loop iteration. */
static uint8_t s_queue[WATCH_FACE_MAX];
static uint8_t s_queue_len = 0;
static uint8_t s_queue_pos = 0;

static lv_obj_t *s_scr      = NULL;
static lv_obj_t *s_carousel = NULL;
static lv_obj_t *s_edit     = NULL;

/* ── Simple area-average RGB565 down-scale ─────────────────────────── */
static void downscale_rgb565(const uint16_t *src, int sw, int sh,
                             uint16_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int iy0 = (y * sh) / dh;
        int iy1 = ((y + 1) * sh) / dh;
        if (iy1 <= iy0) iy1 = iy0 + 1;
        if (iy1 > sh) iy1 = sh;
        for (int x = 0; x < dw; x++) {
            int ix0 = (x * sw) / dw;
            int ix1 = ((x + 1) * sw) / dw;
            if (ix1 <= ix0) ix1 = ix0 + 1;
            if (ix1 > sw) ix1 = sw;
            int r = 0, g = 0, b = 0;
            for (int yy = iy0; yy < iy1; yy++) {
                for (int xx = ix0; xx < ix1; xx++) {
                    uint16_t p = src[yy * sw + xx];
                    r += (p >> 11) & 0x1F;
                    g += (p >> 5) & 0x3F;
                    b += p & 0x1F;
                }
            }
            int n = (iy1 - iy0) * (ix1 - ix0);
            dst[y * dw + x] = (uint16_t)(((r / n) << 11) |
                                         ((g / n) << 5) | (b / n));
        }
    }
}

/* ── Preview generation ────────────────────────────────────────────── */

/* Snapshot a face (the active one, or a temporarily-created off-screen
 * one) and store a down-scaled thumbnail (INNER_W x INNER_H) in PSRAM. */
static bool gen_thumb(uint8_t idx, const watch_face_t *face)
{
    bool is_current = (watch_face_current() == face);
    lv_obj_t *scr = is_current ? watch_face_current_screen() : face->create();
    if (!scr) return false;

    /* Non-active faces show a representative "10:08"-style preview
     * (mirrors how Apple Watch previews faces). */
    if (!is_current) {
        if (face->update_time) face->update_time("10:08", "Monday, 10 August");
        if (face->update_battery) face->update_battery(64, false, false);
    }

    lv_draw_buf_t *snap = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);

    if (!is_current) face->destroy();   /* free the temporary face */

    if (!snap) {
        ESP_LOGW(TAG, "snapshot failed for '%s'", face->name);
        return false;
    }

    int sw = snap->header.w;
    int sh = snap->header.h;
    if (sw <= 0 || sh <= 0) {
        lv_draw_buf_destroy(snap);
        return false;
    }

    uint16_t *thumb = heap_caps_malloc((size_t)INNER_W * INNER_H * 2,
                                       MALLOC_CAP_SPIRAM);
    if (!thumb) {
        lv_draw_buf_destroy(snap);
        return false;
    }

    downscale_rgb565((const uint16_t *)snap->data, sw, sh,
                     thumb, INNER_W, INNER_H);
    lv_draw_buf_destroy(snap);

    s_items[idx].data = thumb;
    memset(&s_items[idx].dsc, 0, sizeof(lv_img_dsc_t));
    s_items[idx].dsc.header.w        = INNER_W;
    s_items[idx].dsc.header.h        = INNER_H;
    s_items[idx].dsc.header.cf       = LV_COLOR_FORMAT_RGB565;
    s_items[idx].dsc.data_size       = (uint32_t)INNER_W * INNER_H * 2;
    s_items[idx].dsc.data            = (const uint8_t *)thumb;
    return true;
}

/* ── Carousel UI ───────────────────────────────────────────────────── */

/* Attach a just-generated thumbnail: drop the spinner and show the
 * preview (fills the inner clip). */
static void attach_thumb(picker_item_t *it)
{
    if (!it->card) return;
    if (it->spinner && lv_obj_is_valid(it->spinner)) {
        lv_obj_delete(it->spinner);
        it->spinner = NULL;
    }
    if (it->img && lv_obj_is_valid(it->img) && it->data) {
        lv_image_set_src(it->img, &it->dsc);
        lv_obj_set_size(it->img, INNER_W, INNER_H);
    }
    lv_obj_invalidate(it->card);
}

/* SquareLine design: fixed-size cards, every non-centered card dimmed.
 * Only touches container opacity when it actually changes. */
static void carousel_update(lv_obj_t *car)
{
    int32_t scroll_x = lv_obj_get_scroll_x(car);
    int idx = (scroll_x + STEP / 2) / STEP;
    if (idx < 0) idx = 0;
    if (idx >= (int)s_count) idx = (int)s_count - 1;
    s_centered = (uint8_t)idx;

    /* Distance is computed in content space: card i sits at i*STEP,
     * the viewport center is at scroll_x + SCR_W/2, and the symmetric
     * horizontal padding cancels out. */
    const int32_t range = SCR_W / 2 + CARD_W / 2 + GAP;
    for (uint8_t i = 0; i < s_count; i++) {
        picker_item_t *it = &s_items[i];
        if (!it->card) continue;
        int32_t dist = (int32_t)((int)i * STEP) - scroll_x;
        if (dist < 0) dist = -dist;
        float t = (float)dist / (float)range;
        if (t > 1.0f) t = 1.0f;

        lv_opa_t opa = (t < 0.5f) ? LV_OPA_COVER : OPA_SIDE_CARD;
        if (lv_obj_get_style_opa(it->card, 0) != opa) {
            lv_obj_set_style_opa(it->card, opa, 0);
            lv_obj_invalidate(it->card);
        }
    }

    /* Edit button: full opacity only when the centred face is editable. */
    if (s_edit && s_count > 0) {
        bool editable = watch_face_editor_supported(s_items[s_centered].face->id);
        lv_opa_t eopa = editable ? LV_OPA_COVER : LV_OPA_40;
        if (lv_obj_get_style_opa(s_edit, 0) != eopa) {
            lv_obj_set_style_opa(s_edit, eopa, 0);
        }
    }
    g_last_activity_us = esp_timer_get_time();
}

static void carousel_scroll_cb(lv_event_t *e)
{
    carousel_update(lv_event_get_target(e));
}

static void card_click_cb(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_target(e);
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_items[i].card == card) {
            if (i == s_centered) {
                watch_face_picker_select(s_items[i].face->id);
            } else {
                lv_obj_scroll_to_x(s_carousel, (int32_t)i * STEP, LV_ANIM_ON);
            }
            return;
        }
    }
}

/* ── Screen build / destroy ────────────────────────────────────────── */

/* "Edit" button — opens the watch-face colour editor for the centred face. */
static void edit_click_cb(lv_event_t *e)
{
    (void)e;
    app_events_request_vibe_click();
    if (s_centered < s_count &&
        watch_face_editor_supported(s_items[s_centered].face->id)) {
        watch_face_editor_open(s_items[s_centered].face->id);
    } else {
        ESP_LOGI(TAG, "Edit not supported for this face");
    }
}

static void build_picker_screen(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Horizontal snap-scroll carousel (full screen; cards centered) */
    s_carousel = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_carousel);
    lv_obj_set_size(s_carousel, SCR_W, SCR_H);
    lv_obj_set_pos(s_carousel, 0, 0);
    lv_obj_set_style_bg_opa(s_carousel, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(s_carousel, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_carousel, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_carousel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_left(s_carousel, (SCR_W - CARD_W) / 2, 0);
    lv_obj_set_style_pad_right(s_carousel, (SCR_W - CARD_W) / 2, 0);
    lv_obj_add_event_cb(s_carousel, carousel_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_carousel, carousel_scroll_cb, LV_EVENT_SCROLL_END, NULL);

    /* Build one card per face with a for loop (SquareLine layout):
     *   container → panel (outline) → clip (rounded) → preview image,
     *   plus a name label above and a spinner until the thumb loads. */
    lv_coord_t card_y = (SCR_H - CARD_H) / 2;
    for (uint8_t i = 0; i < s_count; i++) {
        lv_obj_t *card = lv_obj_create(s_carousel);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, (int32_t)i * STEP, card_y);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(card, OPA_SIDE_CARD, 0);   /* dim unless centered */

        /* Outline panel (black bg + gray border + clip) */
        lv_obj_t *panel = lv_obj_create(card);
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
        lv_obj_set_align(panel, LV_ALIGN_CENTER);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(panel, PANEL_R, 0);
        lv_obj_set_style_bg_color(panel, COLOR_BG, 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(panel, COLOR_OUTLINE, 0);
        lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(panel, CARD_BORDER, 0);
        lv_obj_set_style_clip_corner(panel, true, 0);

        /* Inner rounded clip for the preview */
        lv_obj_t *clip = lv_obj_create(panel);
        lv_obj_remove_style_all(clip);
        lv_obj_set_size(clip, lv_pct(INNER_PCT), lv_pct(INNER_PCT));
        lv_obj_set_align(clip, LV_ALIGN_CENTER);
        lv_obj_remove_flag(clip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(clip, INNER_R, 0);
        lv_obj_set_style_clip_corner(clip, true, 0);

        /* Preview image (src set when the thumbnail is generated) */
        lv_obj_t *img = lv_image_create(clip);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_align(img, LV_ALIGN_CENTER);

        /* System hexagon spinner (cheap) */
        lv_obj_t *spinner = spinner_create(clip, SPINNER_DEFAULT_DIAMETER);
        lv_obj_center(spinner);
        lv_obj_remove_flag(spinner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* Face name above the card (scrolls with it, never dimmed) */
        lv_obj_t *name = lv_label_create(s_carousel);
        lv_obj_set_width(name, CARD_W);
        lv_obj_set_pos(name, (int32_t)i * STEP, NAME_Y);
        lv_label_set_text(name, s_items[i].face->name);
        lv_obj_set_style_text_color(name, COLOR_TEXT, 0);
        lv_obj_set_style_text_font(name, &noto_font_24, 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        s_items[i].card    = card;
        s_items[i].panel   = panel;
        s_items[i].clip    = clip;
        s_items[i].img     = img;
        s_items[i].name    = name;
        s_items[i].spinner = spinner;
        lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, NULL);
    }

    /* "Edit" button (placeholder) — SquareLine look */
    s_edit = lv_button_create(s_scr);
    lv_obj_set_size(s_edit, EDIT_BTN_W, EDIT_BTN_H);
    lv_obj_align(s_edit, LV_ALIGN_BOTTOM_MID, 0, EDIT_BTN_OFF);
    lv_obj_remove_flag(s_edit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_edit, EDIT_BTN_R, 0);
    lv_obj_set_style_bg_color(s_edit, COLOR_OUTLINE, 0);
    lv_obj_set_style_bg_opa(s_edit, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_edit, edit_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *edit_lbl = lv_label_create(s_edit);
    lv_label_set_text(edit_lbl, "Edit");
    lv_obj_center(edit_lbl);
    lv_obj_set_style_text_color(edit_lbl, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(edit_lbl, &noto_font_24, 0);

    /* Start at the current face */
    uint8_t cur_idx = 0;
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_items[i].face == watch_face_current()) { cur_idx = i; break; }
    }
    s_centered = cur_idx;
    lv_obj_scroll_to_x(s_carousel, (int32_t)cur_idx * STEP, LV_ANIM_OFF);
    carousel_update(s_carousel);
}

static void free_picker_resources(void)
{
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_items[i].data) {
            free(s_items[i].data);
            s_items[i].data = NULL;
        }
        s_items[i].card    = NULL;
        s_items[i].panel   = NULL;
        s_items[i].clip    = NULL;
        s_items[i].img     = NULL;
        s_items[i].name    = NULL;
        s_items[i].spinner = NULL;
        s_items[i].face    = NULL;
    }
    s_queue_len = 0;
    s_queue_pos = 0;
}

static void destroy_picker_screen(void)
{
    if (s_scr) lv_obj_delete(s_scr);
    s_scr      = NULL;
    s_carousel = NULL;
    s_edit     = NULL;
    free_picker_resources();
    s_count = 0;
}

/* ── Scale transitions (watch face ⇄ picker preview) ──────────────── */

/* Screen-space rect of the centered preview clip (inside the outline).
 * Used as the shrink/expand target for the transitions. */
#define PREVIEW_X   ((SCR_W - CARD_W) / 2 + (CARD_W - INNER_W) / 2)
#define PREVIEW_Y   ((SCR_H - CARD_H) / 2 + (CARD_H - INNER_H) / 2)
#define PREVIEW_W   INNER_W
#define PREVIEW_H   INNER_H

static lv_obj_t      *s_trans_img = NULL;       /* overlay on lv_layer_top */
static uint8_t       *s_open_snap = NULL;       /* PSRAM copy of the current face */
static lv_img_dsc_t   s_open_snap_dsc;
static uint8_t        s_pending_select_id = 0;  /* face id chosen by the user */
static bool           s_select_pending = false; /* scale-up finished → activate */

static void trans_cleanup(void)
{
    if (s_trans_img) {
        lv_obj_delete(s_trans_img);
        s_trans_img = NULL;
    }
    if (s_open_snap) {
        free(s_open_snap);
        s_open_snap = NULL;
    }
    memset(&s_open_snap_dsc, 0, sizeof(s_open_snap_dsc));
}

static void trans_scale_cb(void *var, int32_t v)
{
    lv_image_set_scale((lv_obj_t *)var, (uint32_t)v);
}

/* Real activation once the select scale-up has finished. */
static void finish_select(uint8_t id)
{
    /* Persist the selection across reboots */
    if (g_config.watch_face_id != id) {
        g_config.watch_face_id = id;
        app_config_save();
    }

    /* Activate face first (destroys old face, creates + loads new screen),
     * then free the picker screen which is no longer active. */
    bool ok = watch_face_activate(id);
    destroy_picker_screen();

    if (ok) {
        g_current_screen = APP_SCREEN_WATCH_FACE;
        app_ui_update_time();
        app_ui_update_battery();
        app_events_request_vibe_click();
        g_last_activity_us = esp_timer_get_time();
    }
}

/* ── Open: the current face shrinks into the centered preview card ── */
static void open_trans_end_cb(lv_anim_t *a)
{
    (void)a;
    trans_cleanup();
    g_last_activity_us = esp_timer_get_time();
}

static void start_open_transition(void)
{
    lv_obj_t *cur = watch_face_current_screen();
    if (!cur) return;

    lv_draw_buf_t *snap = lv_snapshot_take(cur, LV_COLOR_FORMAT_RGB565);
    if (!snap) return;

    /* Own a PSRAM copy so the overlay keeps a stable source after the
     * draw-buf wrapper is released. */
    s_open_snap = heap_caps_malloc(snap->data_size, MALLOC_CAP_SPIRAM);
    if (!s_open_snap) {
        lv_draw_buf_destroy(snap);
        return;
    }
    memcpy(s_open_snap, snap->data, snap->data_size);
    memset(&s_open_snap_dsc, 0, sizeof(s_open_snap_dsc));
    s_open_snap_dsc.header.w      = snap->header.w;
    s_open_snap_dsc.header.h      = snap->header.h;
    s_open_snap_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_open_snap_dsc.data_size     = snap->data_size;
    s_open_snap_dsc.data          = s_open_snap;
    lv_draw_buf_destroy(snap);

    lv_obj_t *img = lv_image_create(lv_layer_top());
    lv_obj_remove_style_all(img);
    lv_obj_set_size(img, SCR_W, SCR_H);
    lv_obj_set_pos(img, 0, 0);
    lv_image_set_src(img, &s_open_snap_dsc);
    lv_image_set_pivot(img, PREVIEW_X + PREVIEW_W / 2, PREVIEW_Y + PREVIEW_H / 2);
    lv_image_set_scale(img, LV_SCALE_NONE);
    s_trans_img = img;

    int32_t to_scale = PREVIEW_W * LV_SCALE_NONE / SCR_W;   /* full → card size */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, img);
    lv_anim_set_exec_cb(&a, trans_scale_cb);
    lv_anim_set_values(&a, LV_SCALE_NONE, to_scale);
    lv_anim_set_time(&a, 280);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, open_trans_end_cb);
    lv_anim_start(&a);
}

/* ── Select: centered preview expands to full screen, then activates ── */
static void select_trans_end_cb(lv_anim_t *a)
{
    (void)a;
    trans_cleanup();
    s_select_pending = true;    /* main loop completes the activation */
}

static void start_select_transition(picker_item_t *it)
{
    lv_obj_t *img = lv_image_create(lv_layer_top());
    lv_obj_remove_style_all(img);
    lv_obj_set_size(img, PREVIEW_W, PREVIEW_H);
    lv_obj_set_pos(img, PREVIEW_X, PREVIEW_Y);
    lv_image_set_src(img, &it->dsc);
    lv_image_set_pivot(img, PREVIEW_W / 2, PREVIEW_H / 2);
    lv_image_set_scale(img, LV_SCALE_NONE);
    s_trans_img = img;

    int32_t to_scale = SCR_W * LV_SCALE_NONE / PREVIEW_W;   /* card → full screen */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, img);
    lv_anim_set_exec_cb(&a, trans_scale_cb);
    lv_anim_set_values(&a, LV_SCALE_NONE, to_scale);
    lv_anim_set_time(&a, 280);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, select_trans_end_cb);
    lv_anim_start(&a);
}

/* ── Public API ────────────────────────────────────────────────────── */

void watch_face_picker_open(void)
{
    if (s_open) return;
    if (g_current_screen != APP_SCREEN_WATCH_FACE) return;
    /* No !g_is_charging guard here on purpose.  While the charging overlay
     * is active g_current_screen == APP_SCREEN_CHARGING (not WATCH_FACE),
     * so the screen check above alone keeps the picker from opening over
     * the charging animation.  Gating on g_is_charging instead locked the
     * picker out whenever the battery happened to be charging. */

    s_count = watch_face_count();
    if (s_count == 0 || s_count > WATCH_FACE_MAX) return;
    memset(s_items, 0, sizeof(s_items));

    /* Face metadata (names) is available instantly — no SPIFFS read. */
    for (uint8_t i = 0; i < s_count; i++) {
        s_items[i].face = watch_face_get(i);
    }

    /* Build the UI immediately: names + spinners on every card. */
    build_picker_screen();
    lv_screen_load(s_scr);

    s_open = true;
    g_current_screen = APP_SCREEN_WATCH_FACE_PICKER;
    g_last_activity_us = esp_timer_get_time();
    app_events_request_vibe_click();
    ESP_LOGI(TAG, "picker opened (%u faces) — thumbs load async", s_count);

    /* Shrink the current face into its preview card (Apple-style). */
    start_open_transition();

    /* Queue preview generation: current face first, then neighbours
     * outward so the visible cards fill in before the far ones. */
    uint8_t cur_idx = 0;
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_items[i].face == watch_face_current()) { cur_idx = i; break; }
    }
    s_queue_len = 0;
    s_queue_pos = 0;
    s_queue[s_queue_len++] = cur_idx;
    for (uint8_t d = 1; d < s_count; d++) {
        int8_t l = (int8_t)cur_idx - (int8_t)d;
        int8_t r = (int8_t)cur_idx + (int8_t)d;
        if (l >= 0)              s_queue[s_queue_len++] = (uint8_t)l;
        if (r < (int8_t)s_count) s_queue[s_queue_len++] = (uint8_t)r;
    }
}

/* Generate the next queued thumbnail (one per main-loop iteration).
 * Called under the LVGL display lock from watch_os.c while the picker is
 * open.  The UI is already visible with a spinner on this card, so the
 * brief block here (SPIFFS decode + snapshot) only stalls that spinner —
 * it never delays the picker from appearing. */
void watch_face_picker_load_next_thumb(void)
{
    if (!s_open) return;
    if (s_queue_pos >= s_queue_len) return;

    uint8_t idx = s_queue[s_queue_pos++];
    if (idx >= s_count) return;

    picker_item_t *it = &s_items[idx];
    if (!it->face) return;

    if (!gen_thumb(idx, it->face)) {
        ESP_LOGW(TAG, "thumb failed for '%s'", it->face->name);
        return;
    }

    /* Swap the spinner for the rendered preview (defensive validity
     * checks — the picker may have been closed mid-generation). */
    if (it->card && lv_obj_is_valid(it->card)) {
        attach_thumb(it);
    }
}

void watch_face_picker_select(uint8_t id)
{
    if (!s_open) return;
    s_open = false;
    ESP_LOGI(TAG, "picker select face id=%d", id);

    /* Scale the centered preview up to full screen, then activate. */
    picker_item_t *it = &s_items[s_centered];
    if (it->data) {
        s_pending_select_id = id;
        start_select_transition(it);
        return;
    }
    /* No preview generated yet — switch instantly. */
    finish_select(id);
}

void watch_face_picker_cancel(void)
{
    if (!s_open) return;
    s_open = false;
    ESP_LOGI(TAG, "picker cancelled");

    /* Kill a running open-transition (face still shrinking into the card). */
    if (s_trans_img) {
        lv_anim_del(s_trans_img, NULL);
        trans_cleanup();
    }

    /* The current face was never destroyed — just load it back */
    lv_obj_t *scr = watch_face_current_screen();
    if (scr) lv_screen_load(scr);
    destroy_picker_screen();

    g_current_screen = APP_SCREEN_WATCH_FACE;
    app_ui_update_time();
    g_last_activity_us = esp_timer_get_time();
}

bool watch_face_picker_is_open(void)
{
    return s_open;
}

/* ── Editor integration ────────────────────────────────────────────── */

/* Return the centred card's preview descriptor (may be NULL before its
 * thumbnail is generated).  Used by the editor's enter transition. */
const lv_img_dsc_t *watch_face_picker_preview_dsc(void)
{
    if (!s_open || s_centered >= s_count) return NULL;
    return s_items[s_centered].data ? &s_items[s_centered].dsc : NULL;
}

/* Screen rect of the centred card's preview (inside its outline). */
void watch_face_picker_preview_rect(lv_area_t *out)
{
    if (!out) return;
    out->x1 = PREVIEW_X;
    out->y1 = PREVIEW_Y;
    out->x2 = PREVIEW_X + PREVIEW_W - 1;
    out->y2 = PREVIEW_Y + PREVIEW_H - 1;
}

/* Close the picker without loading a face (the editor owns the flow now). */
void watch_face_picker_force_close(void)
{
    if (!s_open) return;
    s_open = false;
    if (s_trans_img) {
        lv_anim_del(s_trans_img, NULL);
        trans_cleanup();
    }
    destroy_picker_screen();
}

/* ── Long-press detection (polling) ───────────────────────────────── */

/* Press-start tracking for the polling long-press detector.  We poll the
 * input-device state directly from the main loop instead of relying on
 * LVGL event callbacks (PRESSING is not delivered to indev callbacks, and
 * LONG_PRESSED proved unreliable on this device). */
static int64_t s_press_t0      = 0;
static int32_t s_press_y       = -1;
static bool    s_open_pending  = false;

void watch_face_picker_init(void)
{
    ESP_LOGI(TAG, "long-press detection ready (polling)");
}

/* Polled from the main loop under the LVGL display lock.  Reads the raw
 * input-device state/point — no event callbacks involved. */
void watch_face_picker_process(void)
{
    /* A picker select scale-up finished — complete the activation now
     * (main-loop context, under the LVGL lock; avoids loading a screen
     * from inside an LVGL timer callback). */
    if (s_select_pending) {
        s_select_pending = false;
        finish_select(s_pending_select_id);
    }

    lv_indev_t *indev = hw_touch_get_indev();
    if (!indev) return;

    /* While the notification center is being pulled down or already open,
     * never fire the watch-face picker — the NC owns the gesture. */
    if (app_notification_center_is_open()) {
        s_press_t0 = 0;
        s_open_pending = false;
        return;
    }

    lv_indev_state_t st = lv_indev_get_state(indev);
    int64_t now = esp_timer_get_time();

    if (st == LV_INDEV_STATE_PRESSED) {
        if (s_press_t0 == 0) {
            /* Rising edge — remember where/when the press started */
            lv_point_t pt;
            lv_indev_get_point(indev, &pt);
            s_press_y  = pt.y;
            s_press_t0 = now;
            s_open_pending = false;
        } else if (!s_open_pending &&
                   g_current_screen == APP_SCREEN_WATCH_FACE &&
                   !g_display_off &&
                   !app_notification_center_is_open() &&
                   !(s_press_y >= 0 && s_press_y <= SWIPE_EDGE) &&
                   (now - s_press_t0) > (int64_t)LONG_PRESS_MS * 1000) {
            s_open_pending = true;      /* once per press */
            watch_face_picker_open();   /* called under the display lock */
        }
    } else {
        /* Released — reset */
        s_press_t0 = 0;
        s_open_pending = false;
    }
}
