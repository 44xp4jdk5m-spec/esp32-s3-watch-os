/*
 * Watch OS — JS app loading screen implementation.
 *
 * Black full-screen overlay with the app icon centered and a ring of
 * rotating dots around it.  The dot ring is a port of ElenixOS's
 * eos_loading_spinner in infinite mode: a comet-tail of 24 dots that
 * rotate continuously around the icon.
 */
#include "jsapp_loading.h"

#include <stdlib.h>
#include <string.h>

/* ── Dot-ring geometry ───────────────────────────────────────────── */
#define DOTS_N      24       /* number of dots on the ring            */
#define DOT_MAX_W   8        /* leading (brightest) dot diameter  px  */
#define DOT_MIN_W   2        /* trailing dot diameter             px  */
#define RING_GAP    10       /* gap between icon circumcircle and dots */
#define DEF_ICON_SZ 96       /* assumed icon size when none is present  */
#define ROTATE_MS   2200     /* one full 360° turn                    */

typedef struct {
    lv_obj_t *obj;
    int32_t   rotation;   /* 0.1° units (0..3599) */
    int32_t   ring_r;     /* dot orbit radius in px (icon circumradius + gap) */
    lv_anim_t rotate_anim;
} jsapp_spinner_t;

/* ── Animation / lifecycle ───────────────────────────────────────── */

static void spinner_rot_cb(void *var, int32_t v)
{
    jsapp_spinner_t *sp = (jsapp_spinner_t *)var;
    sp->rotation = v;
    if (sp->obj && lv_obj_is_valid(sp->obj))
        lv_obj_invalidate(sp->obj);
}

static void spinner_delete_cb(lv_event_t *e)
{
    jsapp_spinner_t *sp = (jsapp_spinner_t *)lv_event_get_user_data(e);
    if (!sp) return;
    lv_anim_delete(sp, NULL);
    sp->obj = NULL;
    free(sp);
}

/* ── Drawing ─────────────────────────────────────────────────────── */

/* Tell LVGL the dots extend slightly beyond the widget rect. */
static void spinner_refr_ext_cb(lv_event_t *e)
{
    jsapp_spinner_t *sp = (jsapp_spinner_t *)lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    if (!sp || !obj) return;
    int32_t need = sp->ring_r + DOT_MAX_W / 2;
    int32_t half = lv_obj_get_width(obj) / 2;
    if (need > half)
        lv_event_set_ext_draw_size(e, need - half);
}

/* Draw the rotating comet-tail dot ring (LV_EVENT_DRAW_MAIN_END). */
static void spinner_draw_cb(lv_event_t *e)
{
    jsapp_spinner_t *sp = (jsapp_spinner_t *)lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!sp || !obj) return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t w = lv_area_get_width(&coords);
    int32_t h = lv_area_get_height(&coords);
    if (w < 8 || h < 8) return;

    int32_t cx = coords.x1 + w / 2;
    int32_t cy = coords.y1 + h / 2;
    int32_t ring_r = sp->ring_r;

    /* Leading dot at 12 o'clock; rotation (0.1°) advances clockwise. */
    int16_t lead = (int16_t)(180 - (sp->rotation / 10));

    for (uint16_t i = 0; i < DOTS_N; i++) {
        int16_t ang = lead + (int16_t)((int32_t)i * 360 / DOTS_N);
        ang = ((ang % 360) + 360) % 360;

        int32_t sin_a = lv_trigo_sin(ang);
        int32_t cos_a = lv_trigo_cos(ang);

        int32_t dcx = cx + (ring_r * sin_a) / LV_TRIGO_SIN_MAX;
        int32_t dcy = cy + (ring_r * cos_a) / LV_TRIGO_SIN_MAX;

        /* Comet tail: bright-white leading dot → dark gray trailing */
        uint8_t c = (uint8_t)(255 - (uint32_t)i * 220 / (DOTS_N - 1));

        uint32_t dia = DOT_MAX_W - (uint32_t)i * (DOT_MAX_W - DOT_MIN_W) / (DOTS_N - 1);
        if (dia < 2) dia = 2;

        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.center.x = dcx;
        arc.center.y = dcy;
        arc.radius   = dia / 2;
        arc.width    = dia;
        arc.start_angle = 0;
        arc.end_angle   = 3600;
        arc.color    = lv_color_make(c, c, c);
        arc.rounded  = 1;
        arc.opa      = LV_OPA_COVER;
        lv_draw_arc(layer, &arc);
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

lv_obj_t *jsapp_loading_screen_create(const lv_image_dsc_t *icon)
{
    /* Full-screen black overlay */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 410, 502);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* App icon centered (raw RGB565, natural size) */
    if (icon) {
        lv_obj_t *img = lv_image_create(scr);
        lv_image_set_src(img, icon);
        lv_obj_center(img);
    }

    /* Dot orbit radius = icon circumradius + small gap, so the dots rotate
     * around the icon's circumcenter, just outside its edges. */
    uint32_t iw = icon ? icon->header.w : DEF_ICON_SZ;
    uint32_t ih = icon ? icon->header.h : DEF_ICON_SZ;
    int32_t ring_r = (lv_sqrt32(iw * iw + ih * ih) + 1) / 2 + RING_GAP;

    /* Rotating dot ring around the icon */
    jsapp_spinner_t *sp = (jsapp_spinner_t *)calloc(1, sizeof(*sp));
    if (!sp)
        return scr;
    sp->ring_r = ring_r;

    lv_obj_t *ring = lv_obj_create(scr);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring, ring_r * 2 + DOT_MAX_W * 2, ring_r * 2 + DOT_MAX_W * 2);
    lv_obj_set_style_bg_opa(ring, LV_OPA_MIN, 0);   /* near-invisible bg keeps the draw event alive */
    lv_obj_set_style_bg_color(ring, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(ring, 0, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_center(ring);

    sp->obj = ring;
    lv_obj_add_event_cb(ring, spinner_refr_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, sp);
    lv_obj_add_event_cb(ring, spinner_draw_cb, LV_EVENT_DRAW_MAIN_END, sp);
    lv_obj_add_event_cb(ring, spinner_delete_cb, LV_EVENT_DELETE, sp);

    /* Continuous rotation */
    lv_anim_init(&sp->rotate_anim);
    lv_anim_set_var(&sp->rotate_anim, sp);
    lv_anim_set_exec_cb(&sp->rotate_anim, spinner_rot_cb);
    lv_anim_set_values(&sp->rotate_anim, 0, 3600);
    lv_anim_set_duration(&sp->rotate_anim, ROTATE_MS);
    lv_anim_set_repeat_count(&sp->rotate_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&sp->rotate_anim, lv_anim_path_linear);
    lv_anim_start(&sp->rotate_anim);

    lv_obj_invalidate(ring);
    return scr;
}
