/**
 * @file icon_rearrange.c
 * @brief Honeycomb icon rearrange mode (iOS "wiggle mode").
 *
 * A self-contained flat-grid renderer: no fisheye, no zoom, no touch
 * attraction.  All icons are drawn at 80% and rock left↔right (rotation
 * about an up-left pivot, 3 phase groups).  A long-press entered this mode
 * from the honeycomb menu; a clean tap cancels (no save) and GPIO0 saves +
 * returns.
 *
 * Scroll model (mirrors honeycomb's camera):
 *   screen = virtual - scroll
 *   drag:  scroll -= finger_vector   (content follows the finger)
 * Reordering works in slot-index space: each slot 0..47 maps to a (row,col)
 * via honeycomb_menu_layout_coord(), and a permutation maps slot -> icon id.
 * When the dragged icon hovers over a new slot, the order array is shifted
 * (icons between old and new slot move by one) and every visible icon eases
 * toward its new slot position by the shortest straight-line path.
 */

#include "icon_rearrange.h"
#include "honeycomb_menu.h"
#include "app_config.h"
#include "app_events.h"
#include "hw_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rearrange";

/* ── Display constants (match honeycomb_menu.c) ─────────────────────── */
#define LCD_H_RES           410
#define LCD_V_RES           502
#define ICON_OUTER_RADIUS   (LCD_V_RES / 9)                 /* 55 */
#define ICON_OUTER_DIAMETER (ICON_OUTER_RADIUS * 2)         /* 110 */
#define ICON_INNER_RADIUS   ((ICON_OUTER_RADIUS * 8) / 9)   /* 48 */

#define MAX_ICONS           48
#define REARRANGE_PCT       75                              /* 80% of full size */
#define DRAG_LIFT_Y         80                              /* px above the finger */
#define DRAG_THRESHOLD      8                               /* px before a drag starts */
#define EDGE_ZONE           100                              /* auto-scroll margin */
#define EDGE_MAX_SPEED      16                               /* px per 20ms tick at edge */
#define SHAKE_PERIOD_TICKS  6                              /* full left-right cycle (240ms) */
#define SHAKE_ANGLE         150                              /* ±4.0° in 0.1° units */
#define PIVOT_OFF           0                               /* pivot up-left of centre */                              /* pivot up-left of centre */
#define ANIM_US             200000                          /* reflow move duration (200ms) */

/* ── State ───────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t  *page;
    lv_obj_t  *icons[MAX_ICONS];        /* indexed by icon id */
    uint8_t    order[MAX_ICONS];        /* slot -> icon id (working copy) */
    uint8_t    prov[MAX_ICONS];         /* provisional order while dragging */
    uint8_t    icon_slot[MAX_ICONS];    /* icon id -> current slot */
    int32_t    slot_vx[MAX_ICONS];      /* virtual slot positions */
    int32_t    slot_vy[MAX_ICONS];
    int32_t    render_x[MAX_ICONS];     /* eased virtual pos, per icon id */
    int32_t    render_y[MAX_ICONS];
    int32_t    anim_from_x[MAX_ICONS];  /* move-animation start (virtual) */
    int32_t    anim_from_y[MAX_ICONS];
    int64_t    anim_start_us[MAX_ICONS];/* move-animation start time */
    int32_t    scroll_x, scroll_y;
    int        drag_icon;               /* icon id being dragged, or -1 */
    int        drag_from_slot;
    int        drag_target_slot;
    int        press_icon;              /* icon id currently pressed, or -1 */
    lv_point_t press_start;
    lv_point_t finger;                  /* latest finger position (screen) */
    bool       press_moved;             /* current icon press became a drag */
    bool       drag_started;
    bool       bg_scrolled;             /* background press became a scroll */
    int        bg_move;
    int        tick_count;
    bool       exit_requested;
    bool       exit_save;
    bool       vertical;
    lv_timer_t *fx_timer;
} rearrange_t;

static rearrange_t *s_rr = NULL;
static int         s_src_w = 96, s_src_h = 96;   /* icon source size */
static uint16_t    s_scale_90;                    /* 80% scale */

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int rearrange_nearest_slot(int32_t x, int32_t y)
{
    int best = 0;
    int64_t best_d2 = INT64_MAX;
    for (int i = 0; i < MAX_ICONS; i++) {
        int64_t dx = (int64_t)s_rr->slot_vx[i] - x;
        int64_t dy = (int64_t)s_rr->slot_vy[i] - y;
        int64_t d2 = dx * dx + dy * dy;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

/* Shake angle for an icon.  Three groups (by icon id) rock left↔right at the
 * same speed but start at three different phases, like iOS wiggle but as a
 * rotation instead of a translation.  The pivot sits a few px up-left of the
 * centre, so the rotation also wobbles the icon position slightly. */
static int32_t rearrange_shake_angle(int icon_id)
{
    int group = icon_id % 3;
    int off = group * (SHAKE_PERIOD_TICKS / 3);          /* 0, 4, 8 */
    int ph = (s_rr->tick_count + off) % SHAKE_PERIOD_TICKS;
    int half = SHAKE_PERIOD_TICKS / 2;

    if (ph < half) {
        return -SHAKE_ANGLE + (2 * SHAKE_ANGLE * ph) / half;
    }
    return SHAKE_ANGLE - (2 * SHAKE_ANGLE * (ph - half)) / half;
}

/* Start (or restart) the 200ms move animation for one icon, from its current
 * rendered position to whatever slot it currently occupies. */
static void rearrange_begin_move(int icon_id)
{
    s_rr->anim_from_x[icon_id] = s_rr->render_x[icon_id];
    s_rr->anim_from_y[icon_id] = s_rr->render_y[icon_id];
    s_rr->anim_start_us[icon_id] = esp_timer_get_time();
}

/* Shift the provisional order so the dragged icon occupies drag_target_slot. */
static void rearrange_rebuild_prov(void)
{
    int from = s_rr->drag_from_slot;
    int to   = s_rr->drag_target_slot;

    for (int i = 0; i < MAX_ICONS; i++) s_rr->prov[i] = s_rr->order[i];
    if (to == from) return;

    int drag = s_rr->prov[from];
    if (to > from) {
        for (int i = from; i < to; i++) s_rr->prov[i] = s_rr->prov[i + 1];
    } else {
        for (int i = from; i > to; i--) s_rr->prov[i] = s_rr->prov[i - 1];
    }
    s_rr->prov[to] = drag;
}

/* Rebuild the icon id -> slot reverse map from the provisional order, and
 * start a 200ms move animation for every icon whose slot actually changed. */
static void rearrange_recompute_slots(void)
{
    uint8_t old_slot[MAX_ICONS];
    memcpy(old_slot, s_rr->icon_slot, MAX_ICONS);

    for (int i = 0; i < MAX_ICONS; i++) s_rr->icon_slot[s_rr->prov[i]] = i;

    for (int i = 0; i < MAX_ICONS; i++) {
        if (i == s_rr->drag_icon) continue;
        if (old_slot[i] != s_rr->icon_slot[i]) rearrange_begin_move(i);
    }
}

/* Commit the working order into g_config and write it to SPIFFS. */
static void rearrange_commit(void)
{
    if (!s_rr) return;
    if (s_rr->drag_icon >= 0) {
        memcpy(s_rr->order, s_rr->prov, MAX_ICONS);
    }
    memcpy(g_config.honeycomb_icon_order, s_rr->order, MAX_ICONS);
    app_config_save();
}

static void rearrange_teardown(void)
{
    if (!s_rr) return;
    hw_display_set_brightness(0);
    if (s_rr->fx_timer) {
        lv_timer_delete(s_rr->fx_timer);
        s_rr->fx_timer = NULL;
    }
    if (s_rr->page) {
        lv_anim_delete(s_rr->page, NULL);
        lv_obj_del(s_rr->page);
        s_rr->page = NULL;
    }
    free(s_rr);
    s_rr = NULL;
}

/* Defer the teardown to the next fx tick — safe to call from LVGL events. */
static void rearrange_request_exit(bool save)
{
    if (!s_rr) return;
    s_rr->exit_requested = true;
    s_rr->exit_save = save;
}

static void rearrange_auto_scroll(void)
{
    int fx = s_rr->finger.x;
    int fy = s_rr->finger.y;
    int dist;

    if (fy < EDGE_ZONE) {
        dist = EDGE_ZONE - fy;
        s_rr->scroll_y -= (dist * EDGE_MAX_SPEED) / EDGE_ZONE;
    } else if (fy > LCD_V_RES - EDGE_ZONE) {
        dist = fy - (LCD_V_RES - EDGE_ZONE);
        s_rr->scroll_y += (dist * EDGE_MAX_SPEED) / EDGE_ZONE;
    }

    if (!s_rr->vertical) {
        if (fx < EDGE_ZONE) {
            dist = EDGE_ZONE - fx;
            s_rr->scroll_x -= (dist * EDGE_MAX_SPEED) / EDGE_ZONE;
        } else if (fx > LCD_H_RES - EDGE_ZONE) {
            dist = fx - (LCD_H_RES - EDGE_ZONE);
            s_rr->scroll_x += (dist * EDGE_MAX_SPEED) / EDGE_ZONE;
        }
    }
}

static void rearrange_update_positions(void)
{
    int half = s_src_w / 2;
    int margin = ICON_OUTER_DIAMETER;
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < MAX_ICONS; i++) {
        lv_obj_t *icon = s_rr->icons[i];
        if (!icon) continue;

        if (i == s_rr->drag_icon) {
            /* Dragged icon: floats above the finger, no shake, no easing. */
            int cx = s_rr->finger.x;
            int cy = s_rr->finger.y - DRAG_LIFT_Y;
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(icon, cx - half, cy - half);
            continue;
        }

        int slot = s_rr->icon_slot[i];
        int32_t tx = s_rr->slot_vx[slot];
        int32_t ty = s_rr->slot_vy[slot];
        int32_t scr_x = tx - s_rr->scroll_x;
        int32_t scr_y = ty - s_rr->scroll_y;

        if (scr_x < -margin || scr_x > LCD_H_RES + margin ||
            scr_y < -margin || scr_y > LCD_V_RES + margin) {
            /* Off-screen: snap straight to the slot, don't animate it. */
            s_rr->render_x[i] = tx;
            s_rr->render_y[i] = ty;
            lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        /* Visible: 200ms ease-out move to the slot position. */
        int64_t el = now - s_rr->anim_start_us[i];
        if (el >= ANIM_US || el < 0) {
            s_rr->render_x[i] = tx;
            s_rr->render_y[i] = ty;
        } else {
            int32_t p = (int32_t)((el << 16) / ANIM_US);         /* Q16 0..1 */
            int32_t e = (p * ((2 << 16) - p)) >> 16;             /* ease-out */
            s_rr->render_x[i] = s_rr->anim_from_x[i] +
                (int32_t)(((int64_t)(tx - s_rr->anim_from_x[i]) * e) >> 16);
            s_rr->render_y[i] = s_rr->anim_from_y[i] +
                (int32_t)(((int64_t)(ty - s_rr->anim_from_y[i]) * e) >> 16);
        }

        lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(icon,
                       (int)(s_rr->render_x[i] - s_rr->scroll_x) - half,
                       (int)(s_rr->render_y[i] - s_rr->scroll_y) - half);

        /* Rock left↔right about the up-left pivot (the shake). */
        lv_image_set_rotation(icon, rearrange_shake_angle(i));
    }
}

static void rearrange_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_rr) return;

    if (s_rr->exit_requested) {
        if (s_rr->exit_save) rearrange_commit();
        rearrange_teardown();
        return;
    }

    s_rr->tick_count++;

    if (s_rr->drag_icon >= 0) {
        rearrange_auto_scroll();

        /* The grid may have scrolled under the finger — recompute the hover
         * target so reflow happens even while the finger is held still at
         * the screen edge. */
        int fvx = s_rr->finger.x + s_rr->scroll_x;
        int fvy = s_rr->finger.y + s_rr->scroll_y;
        int target = rearrange_nearest_slot(fvx, fvy);
        if (target != s_rr->drag_target_slot) {
            s_rr->drag_target_slot = target;
            rearrange_rebuild_prov();
            rearrange_recompute_slots();
        }
    }

    rearrange_update_positions();
    lv_obj_invalidate(s_rr->page);
}

/* ── Event callbacks ─────────────────────────────────────────────────── */

static void icon_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int icon_id = (int)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    if (!s_rr || icon_id < 0 || icon_id >= MAX_ICONS) return;

    lv_indev_t *indev = lv_indev_active();

    switch (code) {
    case LV_EVENT_PRESSED:
        s_rr->press_icon = icon_id;
        s_rr->press_moved = false;
        s_rr->drag_started = false;
        if (indev) lv_indev_get_point(indev, &s_rr->press_start);
        if (indev) lv_indev_get_point(indev, &s_rr->finger);
        break;

    case LV_EVENT_PRESSING: {
        if (!indev) break;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_rr->finger = p;

        if (!s_rr->drag_started && s_rr->press_icon == icon_id) {
            int dx = p.x - s_rr->press_start.x;
            int dy = p.y - s_rr->press_start.y;
            if (LV_ABS(dx) > DRAG_THRESHOLD || LV_ABS(dy) > DRAG_THRESHOLD) {
                s_rr->drag_started = true;
                s_rr->press_moved = true;
                s_rr->drag_icon = icon_id;
                s_rr->drag_from_slot = s_rr->icon_slot[icon_id];
                s_rr->drag_target_slot = s_rr->drag_from_slot;
                lv_obj_move_foreground(obj);
                /* Native size (scale NONE) so the radius+clip_corner styles
                 * actually clip the image to a circle in the software
                 * renderer (radius is only honoured on non-transformed
                 * images).  Slightly larger than the 80% neighbours. */
                lv_image_set_pivot(obj, s_src_w / 2, s_src_h / 2);
                lv_image_set_rotation(obj, 0);
                lv_image_set_scale(obj, LV_SCALE_NONE);
            }
        }
        break;
    }

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (s_rr->drag_started && s_rr->drag_icon == icon_id) {
            /* Drop: snap to the nearest slot and commit the provisional
             * order into the working order. */
            if (indev) lv_indev_get_point(indev, &s_rr->finger);
            int fvx = s_rr->finger.x + s_rr->scroll_x;
            int fvy = s_rr->finger.y + s_rr->scroll_y;
            s_rr->drag_target_slot = rearrange_nearest_slot(fvx, fvy);
            rearrange_rebuild_prov();
            rearrange_recompute_slots();
            memcpy(s_rr->order, s_rr->prov, MAX_ICONS);

            /* Restore appearance (back to 80% + up-left shake pivot); the
             * 200ms easing below snaps it into its new slot. */
            lv_image_set_pivot(obj, s_src_w / 2 - PIVOT_OFF, s_src_h / 2 - PIVOT_OFF);
            lv_image_set_rotation(obj, 0);
            lv_image_set_scale(obj, s_scale_90);
            s_rr->render_x[icon_id] = fvx;
            s_rr->render_y[icon_id] = fvy;
            rearrange_begin_move(icon_id);
        } else if (!s_rr->press_moved && code == LV_EVENT_RELEASED) {
            /* Clean tap — cancel without saving. */
            ESP_LOGI(TAG, "Clean tap — cancel rearrange (no save)");
            rearrange_request_exit(false);
        }

        s_rr->press_icon = -1;
        s_rr->drag_started = false;
        s_rr->drag_icon = -1;
        s_rr->press_moved = false;
        break;

    default:
        break;
    }
}

static void page_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (!s_rr) return;

    lv_indev_t *indev = lv_indev_active();

    switch (code) {
    case LV_EVENT_PRESSED:
        s_rr->bg_scrolled = false;
        s_rr->bg_move = 0;
        break;

    case LV_EVENT_PRESSING: {
        if (s_rr->drag_icon >= 0 || !indev) break;
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        if (!s_rr->vertical) s_rr->scroll_x -= v.x;
        s_rr->scroll_y -= v.y;
        s_rr->bg_move += LV_ABS(v.x) + LV_ABS(v.y);
        if (s_rr->bg_move > DRAG_THRESHOLD) s_rr->bg_scrolled = true;
        break;
    }

    case LV_EVENT_RELEASED:
        if (!s_rr->bg_scrolled && s_rr->drag_icon < 0) {
            ESP_LOGI(TAG, "Background tap — cancel rearrange (no save)");
            rearrange_request_exit(false);
        }
        s_rr->bg_scrolled = false;
        s_rr->bg_move = 0;
        break;

    default:
        break;
    }
}

/* ── Vertical entry slot (mirrors honeycomb opening position) ───────── */

static int rearrange_vertical_row_count(int total)
{
    int row = 0, rem = total;
    while (rem > 0) {
        int per = (row % 2 == 0) ? 3 : 4;
        rem -= per;
        if (rem > 0) row++;
    }
    return row;
}

static int rearrange_vertical_entry_idx(int total)
{
    int last = rearrange_vertical_row_count(total);
    int target = (2 < last) ? 2 : last;
    int idx = 0;
    for (int r = 0; r < target; r++) idx += (r % 2 == 0) ? 3 : 4;
    return idx;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void icon_rearrange_create(void)
{
    ESP_LOGI(TAG, "Creating rearrange screen...");

    s_rr = (rearrange_t *)calloc(1, sizeof(rearrange_t));
    if (!s_rr) { ESP_LOGE(TAG, "OOM rearrange"); return; }

    s_rr->vertical = (honeycomb_menu_get_layout_mode() == HONEYCOMB_MODE_VERTICAL_GRID);

    const lv_image_dsc_t *dsc0 = honeycomb_menu_icon_dsc(0);
    s_src_w = dsc0 ? dsc0->header.w : 96;
    s_src_h = dsc0 ? dsc0->header.h : 96;
    s_scale_90  = (uint16_t)((ICON_OUTER_DIAMETER * REARRANGE_PCT / 100) * 256 / s_src_w);

    int n = honeycomb_menu_icon_count();
    if (n > MAX_ICONS) n = MAX_ICONS;

    /* Working order = persisted order. */
    for (int i = 0; i < MAX_ICONS; i++) s_rr->order[i] = g_config.honeycomb_icon_order[i];
    memcpy(s_rr->prov, s_rr->order, MAX_ICONS);

    /* Virtual slot coordinates (same as the honeycomb menu). */
    for (int i = 0; i < n; i++) {
        honeycomb_menu_layout_coord(i, &s_rr->slot_vx[i], &s_rr->slot_vy[i]);
    }

    /* Page: full-screen, black, non-scrollable (we drive scroll manually). */
    lv_obj_t *page = lv_obj_create(lv_screen_active());
    lv_obj_set_style_bg_color(page, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(page, page_cb, LV_EVENT_ALL, NULL);
    s_rr->page = page;

    /* Icons: 80% size, flat, no fisheye, round-clipped via radius (the clip
     * only shows on non-transformed images, i.e. the dragged icon at native
     * scale — the 80% neighbours are transform-scaled so the radius is inert
     * there, but they sit on black so their corners are invisible anyway). */
    for (int slot = 0; slot < n; slot++) {
        int icon_id = s_rr->order[slot];
        lv_obj_t *icon = lv_image_create(page);
        lv_image_set_src(icon, honeycomb_menu_icon_dsc(icon_id));
        lv_image_set_antialias(icon, false);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_image_set_pivot(icon, s_src_w / 2 - PIVOT_OFF, s_src_h / 2 - PIVOT_OFF);
        lv_image_set_rotation(icon, 0);
        lv_image_set_scale(icon, s_scale_90);
        lv_obj_set_style_radius(icon, ICON_INNER_RADIUS, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(icon, true, LV_PART_MAIN);
        lv_obj_add_event_cb(icon, icon_cb, LV_EVENT_ALL,
                            (void *)(uintptr_t)icon_id);
        s_rr->icons[icon_id] = icon;
        s_rr->icon_slot[icon_id] = slot;
        s_rr->render_x[icon_id] = s_rr->slot_vx[slot];
        s_rr->render_y[icon_id] = s_rr->slot_vy[slot];
        s_rr->anim_from_x[icon_id] = s_rr->slot_vx[slot];
        s_rr->anim_from_y[icon_id] = s_rr->slot_vy[slot];
        s_rr->anim_start_us[icon_id] = 0;
    }

    /* Open on the same position the honeycomb menu opens on. */
    if (s_rr->vertical) {
        int entry = rearrange_vertical_entry_idx(n);
        s_rr->scroll_x = 0;
        s_rr->scroll_y = s_rr->slot_vy[entry] - (LCD_V_RES >> 1);
    } else {
        s_rr->scroll_x = s_rr->slot_vx[0] - (LCD_H_RES >> 1);
        s_rr->scroll_y = s_rr->slot_vy[0] - (LCD_V_RES >> 1);
    }

    s_rr->drag_icon = -1;
    s_rr->press_icon = -1;

    rearrange_update_positions();
    lv_obj_invalidate(page);

    s_rr->fx_timer = lv_timer_create(rearrange_tick, 20, NULL);

    /* One frame while dark, then fade the backlight back in. */
    hw_display_set_brightness(0);
    lv_timer_handler();
    honeycomb_menu_fade_in();

    ESP_LOGI(TAG, "Rearrange screen ready (vertical=%d, %d icons)",
             s_rr->vertical, n);
}

void icon_rearrange_save(void)
{
    if (!s_rr) return;
    ESP_LOGI(TAG, "Saving icon order");
    rearrange_commit();
    rearrange_teardown();
}

bool icon_rearrange_is_active(void)
{
    return (s_rr != NULL);
}
