/**
 * @file air_pointer.c
 * @brief Air pointer (motion mouse) implementation — see air_pointer.h.
 *
 * Implementation notes:
 *  - A background task samples the QMI8658 at 50 Hz.  The accelerometer
 *    gravity vector gives the board angle to the earth (roll/pitch), which
 *    is mapped to a relative cursor velocity (cursor keeps moving while
 *    tilted) with a dead zone + exponential smoothing for a controllable
 *    feel.  LVGL objects are only touched under the display lock.
 *  - Clicking is handled by the dedicated double_tap ESP-DL detector
 *    (components/os/double_tap): on a confident finger double-tap it calls
 *    air_pointer_click(), which drives the virtual pointer indev (PRESSED
 *    for ~110 ms) so LVGL resolves a click at the cursor position; the real
 *    touch indev is untouched.
 *  - Edge scroll: when the cursor rests within AP_EDGE_ZONE of an edge, the
 *    scrollable ancestor of the object under the cursor is scrolled at a
 *    speed proportional to how close the cursor is to the edge (mirrors the
 *    honeycomb rearrange edge-scroll).
 *  - While active, the wrist-flick back detector is gated off (both use the
 *    IMU) to avoid interference.
 */

#include "air_pointer.h"

#include "app_config.h"
#include "app_events.h"
#include "hw_display.h"
#include "hw_imu.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <math.h>
#include <string.h>

static const char *TAG = "AIR_PTR";

#define AP_LCD_W            410     /* display width  (px) */
#define AP_LCD_H            502     /* display height (px) */

/* ── Tuning constants ─────────────────────────────────────────────── */
#define AP_SAMPLE_HZ        50
#define AP_TASK_STACK       4096
#define AP_TASK_PRIO        5

#define AP_DEG2RAD          0.01745329252f
/* Tilt -> cursor speed curve (0° = watch flat on a table, MPU x/y axes
 * perpendicular to gravity):
 *   |tilt| < 15°   -> no movement (dead zone)
 *   15° .. 25°     -> slow speed (ramps up to AP_SLOW_MAX_PX)
 *   25° .. 45°     -> speed grows FASTER as the angle grows (quadratic,
 *                     "accelerating" feel — reaches AP_FAST_MAX_PX at 45°)
 *   beyond 45°     -> cursor snaps back to centre and stops moving until
 *                     the wrist returns within range
 * A finger double-tap may only click while |tilt| <= 35° (double_tap.cpp). */
#define AP_TILT_MAX_DEG     90.0f   /* tilt where the pointer recenters */
#define AP_DEADZONE_DEG     15.0f   /* below this: no pointer movement */
#define AP_SLOW_END_DEG     25.0f   /* start of the accelerating zone */
#define AP_SLOW_MAX_PX      1.2f    /* px / 20 ms tick at 25° (start of ramp) */
#define AP_FAST_MAX_PX      5.0f    /* px / 20 ms tick at 45° (max speed) */
#define AP_SMOOTH           0.30f   /* velocity smoothing (0..1, higher=snappier) */
#define AP_X_SIGN          -1.0f    /* dx = -roll */
#define AP_Y_SIGN           1.0f    /* dy = +pitch */
#define AP_CORNER_MM        9.2f    /* screen rounded-corner radius (mm) */
#define AP_CURSOR_MARGIN    24      /* keep the whole cursor on screen (px) */

#define AP_EDGE_ZONE        90      /* auto-scroll margin (px) */
#define AP_EDGE_MAX_SPEED   14      /* px per 20 ms tick at the very edge */

#define AP_CLICK_HOLD_MS    110     /* how long the virtual indev stays PRESSED */

/* ── Module state ─────────────────────────────────────────────────── */
static TaskHandle_t      s_task      = NULL;
static SemaphoreHandle_t s_stopped   = NULL;
static volatile bool     s_stop      = false;
static volatile bool     s_running   = false;

static lv_obj_t         *s_cursor    = NULL;   /* cursor overlay (top layer) */
static lv_indev_t       *s_indev     = NULL;   /* virtual pointer indev */

static volatile int32_t  s_cx, s_cy;           /* cursor position (px) */
static volatile int64_t  s_click_until;        /* >0 while a click is held */

static float             s_svx, s_svy;         /* smoothed cursor velocity */
static float             s_fx,  s_fy;          /* fractional cursor position */
static float             s_corner_px;          /* rounded-corner radius (px) */

static void air_pointer_task(void *arg);

/* ── Virtual pointer indev ──────────────────────────────────────────
 * Report the cursor point; return PRESSED for a short window when a
 * double-tap fires so LVGL processes a normal click at the cursor. */
static void ap_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_cx;
    data->point.y = s_cy;
    data->state   = (esp_timer_get_time() / 1000 < s_click_until)
                        ? LV_INDEV_STATE_PRESSED
                        : LV_INDEV_STATE_RELEASED;
}

/* ── Cursor overlay (dual-tone: white + dark outline → visible on any bg) ── */
static void ap_cursor_create(void)
{
    /* The cursor lives on the SYSTEM layer (the true topmost LVGL layer) so
     * it always draws above app screens AND above other lv_layer_top()
     * objects such as the settings header (which previously hid the cursor).
     * lv_layer_sys() is LVGL's canonical layer for on-screen cursors. */
    if (s_cursor) return;
    s_cursor = lv_obj_create(lv_layer_sys());
    lv_obj_remove_flag(s_cursor, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_cursor, 48, 48);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cursor, 0, 0);
    lv_obj_set_style_pad_all(s_cursor, 0, 0);

    /* Circular ring — white with a dark shadow halo. */
    lv_obj_t *ring = lv_obj_create(s_cursor);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ring, 34, 34);
    lv_obj_center(ring);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_white(), 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(ring, 6, 0);
    lv_obj_set_style_shadow_color(ring, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(ring, LV_OPA_80, 0);

    /* Cross — white bars with a 1 px black border. */
    lv_obj_t *hb = lv_obj_create(s_cursor);
    lv_obj_remove_flag(hb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hb, 42, 3);
    lv_obj_center(hb);
    lv_obj_set_style_bg_color(hb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(hb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hb, 1, 0);
    lv_obj_set_style_border_color(hb, lv_color_black(), 0);

    lv_obj_t *vb = lv_obj_create(s_cursor);
    lv_obj_remove_flag(vb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(vb, 3, 42);
    lv_obj_center(vb);
    lv_obj_set_style_bg_color(vb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(vb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vb, 1, 0);
    lv_obj_set_style_border_color(vb, lv_color_black(), 0);

    lv_obj_set_pos(s_cursor, s_cx - 24, s_cy - 24);
    lv_obj_move_foreground(s_cursor);
}

static void ap_cursor_destroy(void)
{
    if (s_cursor) {
        lv_obj_del(s_cursor);
        s_cursor = NULL;
    }
}

/* ── Edge auto-scroll ───────────────────────────────────────────────
 * The scroll direction matches the pointer's push: at the BOTTOM/right edge
 * the content scrolls DOWN/right (same way the pointer is looking). */
static void ap_edge_scroll(void)
{
    int32_t x = s_cx, y = s_cy;
    int dx = 0, dy = 0;

    if (y < AP_EDGE_ZONE) {
        dy = -((AP_EDGE_ZONE - y) * AP_EDGE_MAX_SPEED) / AP_EDGE_ZONE;
    } else if (y > (AP_LCD_H - AP_EDGE_ZONE)) {
        dy = ((y - (AP_LCD_H - AP_EDGE_ZONE)) * AP_EDGE_MAX_SPEED) / AP_EDGE_ZONE;
    }
    if (x < AP_EDGE_ZONE) {
        dx = -((AP_EDGE_ZONE - x) * AP_EDGE_MAX_SPEED) / AP_EDGE_ZONE;
    } else if (x > (AP_LCD_W - AP_EDGE_ZONE)) {
        dx = ((x - (AP_LCD_W - AP_EDGE_ZONE)) * AP_EDGE_MAX_SPEED) / AP_EDGE_ZONE;
    }

    if (dx == 0 && dy == 0) return;

    lv_point_t pt = { x, y };
    lv_obj_t *obj = lv_indev_search_obj(lv_screen_active(), &pt);
    while (obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) {
        obj = lv_obj_get_parent(obj);
    }
    if (obj) {
        /* Negated so pushing down/right scrolls the content down/right. */
        lv_obj_scroll_by(obj, -dx, -dy, LV_ANIM_ON);
    }
}

/* ── Rounded-rect boundary (physics) ────────────────────────────────
 * The screen has rounded corners (AP_CORNER_MM), so the cursor's travel is
 * bounded by a rounded rectangle.  clamp() returns the nearest valid point
 * (the orthogonal projection; inside an arc it pulls toward the corner
 * centre).  The collision response cancels the outward-normal velocity and
 * keeps the edge-parallel (tangential) component, so the cursor slides along
 * the straight edges AND around the rounded corners (the resultant tilt
 * force follows the arc instead of jamming in a hard corner). */
static void ap_clamp_rounded_rect_f(float *x, float *y, int w, int h,
                                    float corner_r, float margin)
{
    float xw = (float)w, yh = (float)h;
    if (*x < margin) *x = margin;
    if (*x > xw - margin) *x = xw - margin;
    if (*y < margin) *y = margin;
    if (*y > yh - margin) *y = yh - margin;

    float cl = margin + corner_r, cr = xw - margin - corner_r;
    float ct = margin + corner_r, cb = yh - margin - corner_r;
    float ccx, ccy;
    bool corner = false;
    if (*x < cl && *y < ct)       { ccx = cl; ccy = ct; corner = true; }
    else if (*x > cr && *y < ct)  { ccx = cr; ccy = ct; corner = true; }
    else if (*x < cl && *y > cb)  { ccx = cl; ccy = cb; corner = true; }
    else if (*x > cr && *y > cb)  { ccx = cr; ccy = cb; corner = true; }
    if (corner) {
        float dx = *x - ccx, dy = *y - ccy;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > corner_r && dist > 1e-6f) {
            *x = ccx + dx / dist * corner_r;
            *y = ccy + dy / dist * corner_r;
        }
    }
}

/* Tilt -> target cursor speed (px per 20 ms tick) for one axis, applying
 * the dead-zone + two-speed curve. */
static float ap_tilt_speed(float deg)
{
    if (deg < 0.0f) return -ap_tilt_speed(-deg);
    if (deg < AP_DEADZONE_DEG) return 0.0f;
    if (deg < AP_SLOW_END_DEG) {
        return AP_SLOW_MAX_PX * (deg - AP_DEADZONE_DEG) /
               (AP_SLOW_END_DEG - AP_DEADZONE_DEG);
    }
    if (deg > AP_TILT_MAX_DEG) deg = AP_TILT_MAX_DEG;
    /* 25°..45°: accelerating — the cursor speed rises faster as the angle
     * grows (quadratic), giving the "increase speed when tilted more" feel. */
    float t = (deg - AP_SLOW_END_DEG) / (AP_TILT_MAX_DEG - AP_SLOW_END_DEG);
    return AP_SLOW_MAX_PX + (AP_FAST_MAX_PX - AP_SLOW_MAX_PX) * t * t;
}

/* ── Process one IMU sample ───────────────────────────────────────── */
static void ap_process_sample(const hw_imu_data_t *d)
{
    float ax = d->accel_x, ay = d->accel_y, az = d->accel_z;

    /* Board angle to earth.  0° = watch flat face-up (MPU x/y axes
     * perpendicular to gravity).  Each axis = atan2(axis_accel, |other two|)
     * so a flat watch reads 0° on both; range clamped to ±45°. */
    float roll  = atan2f(ay, sqrtf(ax * ax + az * az)) / AP_DEG2RAD;
    float pitch = atan2f(ax, sqrtf(ay * ay + az * az)) / AP_DEG2RAD;

    /* Beyond ±45° the pointer snaps back to centre and stops moving until
     * the wrist returns within range (recentre + hold). */
    if (fabsf(roll) > AP_TILT_MAX_DEG || fabsf(pitch) > AP_TILT_MAX_DEG) {
        s_fx = (float)(AP_LCD_W >> 1);
        s_fy = (float)(AP_LCD_H >> 1);
        s_svx = 0.0f;
        s_svy = 0.0f;
        s_cx = (int32_t)(s_fx + 0.5f);
        s_cy = (int32_t)(s_fy + 0.5f);
        return;
    }

    if (roll  >  AP_TILT_MAX_DEG) roll  =  AP_TILT_MAX_DEG;
    if (roll  < -AP_TILT_MAX_DEG) roll  = -AP_TILT_MAX_DEG;
    if (pitch >  AP_TILT_MAX_DEG) pitch =  AP_TILT_MAX_DEG;
    if (pitch < -AP_TILT_MAX_DEG) pitch = -AP_TILT_MAX_DEG;

    /* Target velocity from the dead-zone + accelerating tilt curve. */
    float tvx = AP_X_SIGN * ap_tilt_speed(roll);
    float tvy = AP_Y_SIGN * ap_tilt_speed(pitch);

    /* Exponential velocity smoothing for a controllable feel. */
    s_svx += (tvx - s_svx) * AP_SMOOTH;
    s_svy += (tvy - s_svy) * AP_SMOOTH;

    /* Integrate, then resolve collisions with the rounded-rect boundary:
     * at an edge the outward-normal velocity is cancelled and only the
     * edge-parallel (tangential) component moves the cursor, so it slides
     * along straight edges and around the rounded corners (the resultant
     * tilt force follows the arc). */
    float fx = s_fx + s_svx;
    float fy = s_fy + s_svy;

    float qx = fx, qy = fy;
    ap_clamp_rounded_rect_f(&qx, &qy, AP_LCD_W, AP_LCD_H,
                            s_corner_px, AP_CURSOR_MARGIN);

    if (qx != fx || qy != fy) {
        /* Outside the boundary: drop the outward-normal velocity. */
        float nx = fx - qx, ny = fy - qy;
        float nlen = sqrtf(nx * nx + ny * ny);
        if (nlen > 1e-4f) {
            nx /= nlen; ny /= nlen;
            float vdotn = s_svx * nx + s_svy * ny;
            if (vdotn > 0.0f) {
                s_svx -= vdotn * nx;
                s_svy -= vdotn * ny;
            }
        }
        /* Slide along the boundary with the remaining tangential velocity. */
        fx = qx + s_svx;
        fy = qy + s_svy;
        ap_clamp_rounded_rect_f(&fx, &fy, AP_LCD_W, AP_LCD_H,
                                s_corner_px, AP_CURSOR_MARGIN);
    }

    s_fx = fx;
    s_fy = fy;
    s_cx = (int32_t)(s_fx + 0.5f);
    s_cy = (int32_t)(s_fy + 0.5f);
    /* (Double-tap clicking is handled by the dedicated double_tap ESP-DL
     *  detector — it calls air_pointer_click() when a tap is recognised and
     *  the watch is within ±35° tilt.) */
}

/* ── Task ─────────────────────────────────────────────────────────── */
static void air_pointer_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / AP_SAMPLE_HZ);

    /* Start the cursor at the centre of the screen. */
    s_cx = AP_LCD_W >> 1;
    s_cy = AP_LCD_H >> 1;
    s_fx = (float)(AP_LCD_W >> 1);
    s_fy = (float)(AP_LCD_H >> 1);
    s_svx = 0.0f;
    s_svy = 0.0f;

    /* Corner radius in px (from the panel mm spec) for the boundary physics. */
    {
        float px_mm_x = (float)AP_LCD_W / 33.09f;
        float px_mm_y = (float)AP_LCD_H / 41.51f;
        float px_mm = (px_mm_x < px_mm_y) ? px_mm_x : px_mm_y;
        s_corner_px = AP_CORNER_MM * px_mm;
        if (s_corner_px < AP_CURSOR_MARGIN + 5) s_corner_px = AP_CURSOR_MARGIN + 5;
    }

    while (!s_stop) {
        bool on = g_config.air_pointer_enabled && !g_display_off && !g_aod_active;

        if (on) {
            hw_imu_data_t d;
            if (hw_imu_read(&d) == ESP_OK) {
                ap_process_sample(&d);
            }
            if (hw_display_lock(30)) {
                if (!s_cursor) {
                    ap_cursor_create();
                    if (!s_indev) {
                        s_indev = lv_indev_create();
                        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
                        lv_indev_set_read_cb(s_indev, ap_indev_read_cb);
                    }
                }
                if (s_cursor) {
                    lv_obj_set_pos(s_cursor, s_cx - 24, s_cy - 24);
                    lv_obj_move_foreground(s_cursor);   /* stay topmost */
                }
                ap_edge_scroll();
                hw_display_unlock();
            }
        } else if (s_cursor || s_indev) {
            if (hw_display_lock(30)) {
                ap_cursor_destroy();
                if (s_indev) {
                    lv_indev_delete(s_indev);
                    s_indev = NULL;
                }
                hw_display_unlock();
            }
        }

        vTaskDelayUntil(&last, period);
    }

    if (s_cursor || s_indev) {
        if (hw_display_lock(50)) {
            ap_cursor_destroy();
            if (s_indev) { lv_indev_delete(s_indev); s_indev = NULL; }
            hw_display_unlock();
        }
    }

    s_running = false;
    if (s_stopped) xSemaphoreGive(s_stopped);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t air_pointer_init(void)
{
    if (!s_stopped) s_stopped = xSemaphoreCreateBinary();
    return s_stopped ? ESP_OK : ESP_ERR_NO_MEM;
}

bool air_pointer_is_enabled(void)
{
    return g_config.air_pointer_enabled;
}

bool air_pointer_is_active(void)
{
    return s_running;
}

void air_pointer_click(void)
{
    /* Drive the virtual pointer indev PRESSED for a short window so LVGL
     * resolves a normal click at the current cursor position.  Called by the
     * double-tap ESP-DL detector (different task). */
    s_click_until = esp_timer_get_time() / 1000 + AP_CLICK_HOLD_MS;
}

esp_err_t air_pointer_start(void)
{
    if (s_task) return ESP_OK;   /* already running */
    s_stop = false;
    BaseType_t ok = xTaskCreatePinnedToCore(air_pointer_task, "air_ptr",
                                            AP_TASK_STACK, NULL,
                                            AP_TASK_PRIO, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    ESP_LOGI(TAG, "Air pointer started");
    return ESP_OK;
}

void air_pointer_stop(void)
{
    if (!s_task) { s_stop = true; return; }
    if (s_task == xTaskGetCurrentTaskHandle()) { s_stop = true; return; }
    s_stop = true;
    if (s_stopped && xSemaphoreTake(s_stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "timed out waiting for task to stop");
        return;
    }
    s_task = NULL;
    s_running = false;
}
