/*
 * Watch OS — shutdown / power-off confirmation screen.
 *
 * Layout adopted from the SquareLine reference (tmp/shutdown): the current
 * screen is blurred (frosted glass) as the backdrop, a "Cancel" pill
 * top-left, and a power-off slider bottom-right made of a track + knob with
 * the power icon (loaded from SPIFFS).  Dragging the knob right shrinks the
 * track width from SLIDER_MAX_W (350) to SLIDER_MIN_W (106); the display
 * brightness tracks the width.  On release it snaps to the nearest end:
 * past the midpoint → snap to min, brightness 0, then hw_shutdown() (which
 * performs the AXP2101 physical POK-hold power-off).
 *
 * The whole thing is an OVERLAY on lv_layer_top(), so the previous screen
 * stays rendered underneath and is blurred behind it.
 */
#include "shutdown_ui.h"

#include "app_config.h"
#include "app_events.h"
#include "app_image_loader.h"
#include "fonts.h"
#include "hw_display.h"
#include "hw_init.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "SHUTDOWN";

/* ── Screen geometry ──────────────────────────────────────────────── */
#define SCR_W       410
#define SCR_H       502

/* ── Power-off slider ─────────────────────────────────────────────── */
#define SLIDER_MAX_W 350    /* track width at rest (SquareLine ref)  */
#define SLIDER_MIN_W 106    /* track width when fully dragged → off  */

/* ── Frosted-glass backdrop ───────────────────────────────────────── */
#define BLUR_RADIUS  20     /* smooth (precision) blur radius        */
#define OVERLAY_DIM  150    /* semi-transparent dark over the blur   */
#define SLIDER_H     100
#define SLIDER_RIGHT 30     /* x offset from the screen's right edge */
#define SLIDER_BOTTOM 30    /* y offset from the screen's bottom edge */
#define KNOB_W       94
#define KNOB_PAD     6      /* knob x offset inside the track        */
#define SNAP_MS      160    /* snap animation duration               */

#define POWER_ICON_PATH "/spiffs/usr/share/shutdown/power_icon.bin"
#define POWER_ICON_W    60
#define POWER_ICON_H    61

/* ── State ────────────────────────────────────────────────────────── */
static lv_obj_t        *s_scr          = NULL;   /* top-layer overlay */
static app_screen_t     s_prev_screen_id = APP_SCREEN_WATCH_FACE;
static lv_image_dsc_t  *s_icon         = NULL;

static lv_obj_t        *s_track        = NULL;   /* slider container  */
static lv_obj_t        *s_label        = NULL;   /* "POWER OFF" label */
static bool             s_dragging     = false;
static int32_t          s_drag_start_x = 0;      /* pointer x at press */
static int32_t          s_width_start  = 0;      /* track width at press */
static uint8_t          s_last_bright   = 0xFF;  /* dedupe SPI writes */

/* ── Brightness / label from width ────────────────────────────────── */

/* Map track width → display brightness.  MAX_W → the user's configured
 * brightness, MIN_W → 0. */
static uint8_t brightness_from_width(int32_t w)
{
    int32_t pct = (int32_t)g_config.brightness * (w - SLIDER_MIN_W)
                  / (SLIDER_MAX_W - SLIDER_MIN_W);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

static void apply_width(int32_t w)
{
    if (w > SLIDER_MAX_W) w = SLIDER_MAX_W;
    if (w < SLIDER_MIN_W) w = SLIDER_MIN_W;

    lv_obj_set_width(s_track, w);

    /* Brightness follows the width (SPI command — dedupe same values). */
    uint8_t b = brightness_from_width(w);
    if (b != s_last_bright) {
        s_last_bright = b;
        hw_display_set_brightness(b);
    }

    /* "POWER OFF" label fades out as the track shrinks. */
    lv_opa_t opa = (lv_opa_t)(255 * (w - SLIDER_MIN_W) / (SLIDER_MAX_W - SLIDER_MIN_W));
    lv_obj_set_style_text_opa(s_label, opa, 0);
}

/* ── Slider snap / shutdown ───────────────────────────────────────── */

static void snap_exec_cb(void *var, int32_t v)
{
    (void)var;
    apply_width(v);
}

static void snap_min_ready_cb(lv_anim_t *a)
{
    (void)a;
    ESP_LOGW(TAG, "Power-off slider at min → full shutdown");
    /* Brightness is already 0 (width == min); power the whole watch off. */
    hw_shutdown();
}

static void snap_slider(void)
{
    int32_t cur = lv_obj_get_width(s_track);
    bool to_min = cur <= (SLIDER_MAX_W + SLIDER_MIN_W) / 2;

    int32_t target = to_min ? SLIDER_MIN_W : SLIDER_MAX_W;
    ESP_LOGI(TAG, "release at width %ld → snap to %s", (long)cur,
             to_min ? "MIN (power off)" : "MAX");

    lv_anim_delete(s_track, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_track);
    lv_anim_set_exec_cb(&a, snap_exec_cb);
    lv_anim_set_values(&a, cur, target);
    lv_anim_set_duration(&a, SNAP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (to_min)
        lv_anim_set_completed_cb(&a, snap_min_ready_cb);
    lv_anim_start(&a);
}

/* ── Slider drag events (on the knob) ─────────────────────────────── */

static void slider_evt_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt;
    if (indev) lv_indev_get_point(indev, &pt);

    switch (code) {
    case LV_EVENT_PRESSED:
        s_dragging = true;
        s_drag_start_x = pt.x;
        s_width_start  = lv_obj_get_width(s_track);
        lv_anim_delete(s_track, NULL);
        break;

    case LV_EVENT_PRESSING:
        if (!s_dragging) break;
        /* Drag right (dx > 0) → track width decreases. */
        apply_width(s_width_start - (pt.x - s_drag_start_x));
        break;

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (!s_dragging) break;
        s_dragging = false;
        snap_slider();
        break;

    default:
        break;
    }
}

/* ── Cancel button ────────────────────────────────────────────────── */

static void cancel_evt_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
        shutdown_ui_dismiss();
}

/* ── Public API ───────────────────────────────────────────────────── */

void shutdown_ui_show(void)
{
    if (s_scr && lv_obj_is_valid(s_scr))
        return;

    ESP_LOGI(TAG, "Showing shutdown screen");

    s_prev_screen_id = g_current_screen;
    s_last_bright = 0xFF;

    /* Power icon from SPIFFS (I8 60×61) — knob glyph. */
    s_icon = app_img_load_raw(POWER_ICON_PATH, POWER_ICON_W, POWER_ICON_H,
                              LV_COLOR_FORMAT_I8);
    if (!s_icon)
        ESP_LOGW(TAG, "power icon not loaded");

    /* ── Frosted-glass overlay on the TOP LAYER ────────────────────
     * The current screen keeps rendering underneath; this overlay blurs it
     * (LV_BLUR_QUALITY_PRECISION = smooth, no pixel skipping) and dims it
     * with a semi-transparent dark bg.  Clickable so touches don't fall
     * through to the underlying screen. */
    s_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_scr);              /* drop default container styles */
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_scr, SCR_W, SCR_H);
    lv_obj_set_style_blur_radius(s_scr, BLUR_RADIUS, 0);
    lv_obj_set_style_blur_quality(s_scr, LV_BLUR_QUALITY_PRECISION, 0);
    lv_obj_set_style_radius(s_scr, 100, 0);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, OVERLAY_DIM, 0);
    lv_obj_set_style_clip_corner(s_scr, true, 0);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);   /* swallow touches */

    /* ── Cancel button (top-left) ────────────────────────────────── */
    lv_obj_t *cancel = lv_button_create(s_scr);
    lv_obj_set_size(cancel, 120, 52);   /* height +50% from SquareLine 35 */
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 30, 30);
    lv_obj_remove_flag(cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(cancel, 50, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x212121), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(cancel, cancel_evt_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(cancel_label, &noto_font_24, 0);
    lv_obj_center(cancel_label);

    /* ── Power-off slider (bottom-right) ─────────────────────────── */
    s_track = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_track);
    lv_obj_set_size(s_track, SLIDER_MAX_W, SLIDER_H);
    lv_obj_align(s_track, LV_ALIGN_BOTTOM_RIGHT, -SLIDER_RIGHT, -SLIDER_BOTTOM);
    lv_obj_remove_flag(s_track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Track background pill */
    lv_obj_t *bg = lv_obj_create(s_track);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bg, lv_pct(100), lv_pct(100));
    lv_obj_align(bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(bg, 100, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x212121), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_opa(bg, 250, 0);

    /* "POWER OFF" label */
    s_label = lv_label_create(s_track);
    lv_label_set_text(s_label, "POWER\nOFF");
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_label, &noto_font_24, 0);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_label);

    /* Knob (draggable) */
    lv_obj_t *knob = lv_obj_create(s_track);
    lv_obj_set_size(knob, KNOB_W, KNOB_W);
    lv_obj_align(knob, LV_ALIGN_LEFT_MID, KNOB_PAD, 0);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_add_event_cb(knob, slider_evt_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(knob, slider_evt_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(knob, slider_evt_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(knob, slider_evt_cb, LV_EVENT_PRESS_LOST, NULL);

    /* Power icon inside the knob */
    if (s_icon) {
        lv_obj_t *img = lv_image_create(knob);
        lv_image_set_src(img, s_icon);
        lv_obj_center(img);
    }

    /* The overlay floats on the top layer — the previous screen stays
     * active underneath (that's what gets blurred). */
    g_current_screen = APP_SCREEN_SHUTDOWN;
    ESP_LOGI(TAG, "Shutdown screen active");
}

void shutdown_ui_dismiss(void)
{
    if (!s_scr || !lv_obj_is_valid(s_scr))
        return;

    ESP_LOGI(TAG, "Shutdown screen dismissed");

    /* Restore brightness in case the drag changed it. */
    if (s_last_bright != g_config.brightness) {
        s_last_bright = g_config.brightness;
        hw_display_set_brightness(g_config.brightness);
    }

    /* The overlay is on the top layer, so the underlying screen never
     * changed — just remove the overlay. */
    lv_obj_del(s_scr);
    s_scr = NULL;

    if (s_icon) {
        app_img_free(s_icon);
        s_icon = NULL;
    }
    if (g_current_screen == APP_SCREEN_SHUTDOWN)
        g_current_screen = s_prev_screen_id;
}

bool shutdown_ui_is_active(void)
{
    return s_scr != NULL && lv_obj_is_valid(s_scr);
}
