/**
 * @file ui_progress_ring.c
 * @brief Reusable radial progress ring implementation.
 *
 * Draws discrete radial bars via LV_EVENT_DRAW_MAIN_END + lv_draw_line
 * (same technique as the ElenixOS loading spinner).  Bar 0 is at 12
 * o'clock; bars below the current progress are white, the rest gray.
 * Progress never needs a bitmap — just a redraw of ~N thin lines.
 */

#include "ui_progress_ring.h"
#include "esp_log.h"
#include <stdlib.h>

#define RING_DEFAULT_BARS   180        /* default number of bars         */
#define RING_INNER_RATIO     43        /* inner radius, % of min(w,h)    */
#define RING_OUTER_RATIO     50        /* outer radius, % of min(w,h)    */
#define RING_BAR_WIDTH        3        /* bar line width in px           */
#define RING_COLOR_IDLE    0x404040    /* idle (gray) bar color          */
#define RING_COLOR_ACTIVE  0xFFFFFF    /* active (white) bar color       */

/* Widget private state, attached to the object as user data. */
typedef struct {
    uint16_t bars;
    uint16_t pct;
    uint8_t  bar_width;
} ring_ctx_t;

/* Report how far the bars extend beyond the widget rect (avoid clipping). */
static void ring_refr_ext_draw_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    int32_t d = (w < h) ? w : h;
    int32_t outer_r = (d * RING_OUTER_RATIO) / 100;
    int32_t overflow = outer_r - d / 2;
    if (overflow > 0) lv_event_set_ext_draw_size(e, overflow);
}

/* Draw the radial bars: bar 0 at 12 o'clock, fill clockwise. */
static void ring_draw_cb(lv_event_t *e)
{
    lv_obj_t   *obj   = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    ring_ctx_t *ctx   = (ring_ctx_t *)lv_obj_get_user_data(obj);
    if (!ctx) return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t w = lv_area_get_width(&coords);
    int32_t h = lv_area_get_height(&coords);
    if (w < 8 || h < 8) return;

    int32_t cx = coords.x1 + w / 2;
    int32_t cy = coords.y1 + h / 2;
    int32_t d  = (w < h) ? w : h;
    int32_t inner_r = (d * RING_INNER_RATIO) / 100;
    int32_t outer_r = (d * RING_OUTER_RATIO) / 100;

    uint16_t active = (uint16_t)(((uint32_t)ctx->pct * ctx->bars) / 100);
    if (active > ctx->bars) active = ctx->bars;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.width = ctx->bar_width;
    line_dsc.opa   = LV_OPA_COVER;

    for (uint16_t i = 0; i < ctx->bars; i++) {
        int16_t angle = (int16_t)(180 - ((int32_t)i * 360) / ctx->bars);
        int32_t sin_a = lv_trigo_sin(angle);
        int32_t cos_a = lv_trigo_cos(angle);

        line_dsc.color = (i < active) ? lv_color_hex(RING_COLOR_ACTIVE)
                                      : lv_color_hex(RING_COLOR_IDLE);
        line_dsc.p1.x = cx + (inner_r * sin_a) / LV_TRIGO_SIN_MAX;
        line_dsc.p1.y = cy + (inner_r * cos_a) / LV_TRIGO_SIN_MAX;
        line_dsc.p2.x = cx + (outer_r * sin_a) / LV_TRIGO_SIN_MAX;
        line_dsc.p2.y = cy + (outer_r * cos_a) / LV_TRIGO_SIN_MAX;

        lv_draw_line(layer, &line_dsc);
    }
}

/* Auto-cleanup: free the private state when the widget dies. */
static void ring_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    ring_ctx_t *ctx = (ring_ctx_t *)lv_obj_get_user_data(obj);
    if (ctx) {
        free(ctx);
        lv_obj_set_user_data(obj, NULL);
    }
}

lv_obj_t *ui_progress_ring_create(lv_obj_t *parent, int32_t size, uint16_t bars)
{
    ring_ctx_t *ctx = (ring_ctx_t *)calloc(1, sizeof(ring_ctx_t));
    if (!ctx) return NULL;
    ctx->bars      = bars ? bars : RING_DEFAULT_BARS;
    ctx->pct       = 0;
    ctx->bar_width = RING_BAR_WIDTH;

    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_set_size(ring, size, size);
    /* Near-invisible bg (1/255) so LVGL doesn't skip this widget; the
     * actual bars are drawn on top via LV_EVENT_DRAW_MAIN_END. */
    lv_obj_set_style_bg_opa(ring, LV_OPA_MIN, 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(ring, 0, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(ring, ctx);
    lv_obj_add_event_cb(ring, ring_refr_ext_draw_size_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_add_event_cb(ring, ring_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(ring, ring_delete_cb, LV_EVENT_DELETE, NULL);
    return ring;
}

void ui_progress_ring_set_progress(lv_obj_t *ring, uint16_t pct)
{
    ring_ctx_t *ctx = ring ? (ring_ctx_t *)lv_obj_get_user_data(ring) : NULL;
    if (!ctx) return;
    if (pct > 100) pct = 100;
    if (ctx->pct != pct) {
        ctx->pct = pct;
        lv_obj_invalidate(ring);
    }
}

void ui_progress_ring_set_bar_width(lv_obj_t *ring, uint8_t width)
{
    ring_ctx_t *ctx = ring ? (ring_ctx_t *)lv_obj_get_user_data(ring) : NULL;
    if (!ctx) return;
    if (width < 1) width = 1;
    if (width > 16) width = 16;
    if (ctx->bar_width != width) {
        ctx->bar_width = width;
        lv_obj_invalidate(ring);
    }
}
