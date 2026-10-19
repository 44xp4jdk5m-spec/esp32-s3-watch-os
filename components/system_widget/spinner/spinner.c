/**
 * @file spinner.c
 * @brief Reusable hexagon-dot spinner system widget (LVGL v9).
 *
 * Six white dots form a hexagon (dot[0]..dot[5]).  The dots never move;
 * a periodic timer rotates the opacity pattern [100,70,50,30,20,50]
 * around the ring so it looks like the wheel is spinning.
 *
 * The timer is stored in the object's user data and released by a
 * LV_EVENT_DELETE handler, so the widget is fully self-contained: apps
 * only need to create it (and optionally pause it).
 */

#include "spinner.h"
#include <stdlib.h>

/* ── Geometry ─────────────────────────────────────────────────────── */
/* Dots are deliberately large so they stay clearly visible even when the
 * settings list scroll effect scales the whole widget down to ~70%. */
#define SPINNER_DOT_DIAMETER   18    /* diameter of each dot in px   */
#define SPINNER_UPDATE_MS     120    /* animation tick period (ms) — slower = no flicker */

/* Opacity pattern for dot[0]..dot[5] in PERCENT (0-100).  Rotated by one
 * slot each tick. */
static const uint8_t s_spinner_opa[6] = { 100, 70, 50, 30, 20, 50 };

/* Convert a percent (0-100) to an LVGL opacity (0-255).  LVGL opacity is
 * 0..255 (LV_OPA_COVER=255), NOT percent — passing 100 raw renders at only
 * 100/255 ~= 39 %, so the dots never looked solid white. */
#define SPINNER_OPA(pct)   ((lv_opa_t)((pct) * LV_OPA_COVER / 100))

/* Widget private state, attached to the object as user data. */
typedef struct {
    lv_timer_t *timer;      /* NULL while paused                      */
    lv_obj_t   *dots[6];    /* dot[0]..dot[5]                         */
    uint8_t     phase;      /* current rotation offset (0..5)         */
} spinner_data_t;

/* ── Animation ────────────────────────────────────────────────────── */
static void spinner_timer_cb(lv_timer_t *t)
{
    spinner_data_t *d = (spinner_data_t *)lv_timer_get_user_data(t);
    if (!d) return;

    d->phase = (d->phase + 1) % 6;
    for (int i = 0; i < 6; i++) {
        lv_obj_set_style_bg_opa(d->dots[i], SPINNER_OPA(s_spinner_opa[(i + d->phase) % 6]), 0);
    }
}

/* Auto-cleanup: free the state and kill the timer when the object dies. */
static void spinner_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    spinner_data_t *d = (spinner_data_t *)lv_obj_get_user_data(obj);
    if (d) {
        if (d->timer) lv_timer_del(d->timer);
        free(d);
        lv_obj_set_user_data(obj, NULL);
    }
}

/* ── Public API ───────────────────────────────────────────────────── */
lv_obj_t *spinner_create(lv_obj_t *parent, lv_coord_t diameter)
{
    /* Keep the dot size fixed and grow the ring radius with the diameter. */
    lv_coord_t dot_d = SPINNER_DOT_DIAMETER;
    lv_coord_t ring_r = (diameter - dot_d) / 2;
    if (ring_r < 4) ring_r = 4;

    spinner_data_t *d = (spinner_data_t *)calloc(1, sizeof(spinner_data_t));
    if (!d) return NULL;

    lv_obj_t *spinner = lv_obj_create(parent);
    lv_obj_set_size(spinner, diameter, diameter);
    lv_obj_set_style_bg_opa(spinner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spinner, 0, 0);
    lv_obj_set_style_pad_all(spinner, 0, 0);
    lv_obj_set_style_radius(spinner, 0, 0);
    lv_obj_remove_flag(spinner, LV_OBJ_FLAG_SCROLLABLE);

    lv_coord_t cx = diameter / 2;
    lv_coord_t cy = diameter / 2;

    for (int i = 0; i < 6; i++) {
        /* Angles in DEGREES (LVGL lv_trigo_sin/cos take 0..360, not 0.1°);
         * dot[0] at the top, going clockwise. */
        int16_t angle = (int16_t)(90 + i * 60);
        lv_coord_t x = cx + (ring_r * lv_trigo_cos(angle)) / LV_TRIGO_SIN_MAX - dot_d / 2;
        lv_coord_t y = cy - (ring_r * lv_trigo_sin(angle)) / LV_TRIGO_SIN_MAX - dot_d / 2;

        lv_obj_t *dot = lv_obj_create(spinner);
        lv_obj_set_size(dot, dot_d, dot_d);
        lv_obj_set_pos(dot, x, y);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(dot, SPINNER_OPA(s_spinner_opa[(i + d->phase) % 6]), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        d->dots[i] = dot;
    }

    lv_obj_set_user_data(spinner, d);
    lv_obj_add_event_cb(spinner, spinner_delete_cb, LV_EVENT_DELETE, NULL);

    d->timer = lv_timer_create(spinner_timer_cb, SPINNER_UPDATE_MS, d);
    return spinner;
}

void spinner_set_active(lv_obj_t *spinner, bool active)
{
    spinner_data_t *d = (spinner_data_t *)lv_obj_get_user_data(spinner);
    if (!d) return;

    if (active) {
        if (!d->timer) {
            d->timer = lv_timer_create(spinner_timer_cb, SPINNER_UPDATE_MS, d);
        }
    } else {
        if (d->timer) {
            lv_timer_del(d->timer);
            d->timer = NULL;
        }
    }
}
