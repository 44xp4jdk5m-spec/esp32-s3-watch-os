/**
 * @file activity_ui.c
 * @brief Activity app — rings + Move / Exercise / Stand pages.
 *
 * A faithful port of the user's SquareLine designs in /my_ui
 * (ui_activity / ui_move / ui_exercise), plus a new Stand page:
 *
 *   - Entry lands on the rings ("Activity") page with a real-time clock.
 *     The three rings animate up from 0 to their current values.
 *   - Swiping up / down cycles the pages in the order
 *         [ activity → move → exercise → stand ].
 *     Page switches use the backlight-driven fade (same look as the
 *     honeycomb menu entry): black out, swap the screen, ramp the
 *     brightness back up — no per-pixel blending.
 *   - The move / exercise / stand metrics come from a dedicated IMU task
 *     that samples the QMI8658 at 50 Hz while the app is open (steps via
 *     dynamic-accel peak detection, exercise via brisk minutes, stand via
 *     wrist-upright hours).  The chart is a rolling 24 h window whose
 *     right edge is "now", so the current 6 h section is always last.
 *   - GPIO0 (handled by the OS via activity_ui_handle_back) exits to the
 *     menu.
 *
 * Memory model: the app context is allocated from PSRAM on create() and
 * freed on destroy(); the screens are LVGL objects (PSRAM-backed LVGL
 * allocator) deleted on destroy().  The OS never references this module
 * until the Activity icon is tapped.
 */

#include "activity_ui.h"

#include "activity_pages.h"
#include "app_events.h"
#include "app_config.h"
#include "hw_display.h"
#include "hw_imu.h"
#include "hw_rtc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ACT";

/* ── Swipe / animation constants ───────────────────────────────────── */
#define ACT_SWIPE_THRESHOLD    40    /* px of vertical travel to count as a swipe */
#define ACT_SWIPE_RATIO        2     /* |dy| must beat |dx| by at least this      */
#define ACT_RING_ANIM_MS       700   /* rings count-up duration                   */
#define ACT_FADE_MS            220   /* backlight fade duration                   */

/* ── Activity goals ────────────────────────────────────────────────── */
#define ACT_MOVE_GOAL_KCAL      500   /* move ring goal (kcal)      */
#define ACT_EXERCISE_GOAL_MIN   30    /* exercise ring goal (min)   */
#define ACT_STAND_GOAL_HRS      12    /* stand ring goal (hours)    */
#define ACT_KCAL_PER_STEP       0.04f /* rough kcal per step        */

/* ── IMU sampling task ─────────────────────────────────────────────── */
#define ACT_IMU_SAMPLE_HZ       50
#define ACT_IMU_TASK_NAME       "act_imu"
#define ACT_IMU_TASK_STACK      4096
#define ACT_IMU_TASK_PRIO       5

/* Step detection — dynamic (gravity-free) acceleration peak threshold
 * (m/s²) with a debounce so one stride counts once.  Tune on-device.   */
#define ACT_STEP_THRESH         2.2f
#define ACT_STEP_DEBOUNCE_MS    250

/* Exercise: a minute with at least this many steps counts as active.  */
#define ACT_EXERCISE_STEPS_MIN  30

/* Stand: an hour counts as "stood" if this many of its minutes had the
 * wrist upright (gravity mostly in the horizontal plane).             */
#define ACT_STAND_UP_MINUTES    30

/* ── App context (allocated in PSRAM, freed on exit) ───────────────── */
typedef struct {
    activity_page_ui_t pages[ACTIVITY_PAGE_COUNT];
    int       cur;             /* active page index                    */
    lv_obj_t *prev_screen;     /* screen to restore on exit            */
    lv_timer_t *timer;         /* 1 s live-clock timer                 */
    lv_point_t press_start;    /* swipe: press origin                  */
    lv_point_t press_cur;      /* swipe: latest press position         */
    uint8_t    fade_var;       /* lv_anim var for the brightness fade  */

    /* IMU sampling task (exists only while the app is open) */
    TaskHandle_t imu_task;
    volatile bool imu_run;

    /* Metrics accumulated by the IMU task (read by the UI timer) */
    volatile int steps;
    volatile int exercise_min;
    volatile int stand_hours;

    /* Last displayed values — to skip redundant UI updates */
    int last_kcal, last_ex_min, last_stand_hours;
} activity_ctx_t;

static activity_ctx_t *s_ctx = NULL;

/* ══════════════════════════════════════════════════════════════════════
 * Ring count-up animation — one progress anim (0→100) drives all three
 * rings so they each land on their own target value.
 * ══════════════════════════════════════════════════════════════════════ */
static void activity_rings_exec_cb(void *var, int32_t v)
{
    activity_ctx_t *c = (activity_ctx_t *)var;

    /* Target the LIVE metric percentages (not fixed demo values). */
    int kcal = (int)(c->steps * ACT_KCAL_PER_STEP);
    int move_pct  = (kcal * 100) / ACT_MOVE_GOAL_KCAL;
    int ex_pct    = (c->exercise_min * 100) / ACT_EXERCISE_GOAL_MIN;
    int stand_pct = (c->stand_hours * 100) / ACT_STAND_GOAL_HRS;
    if (move_pct > 100)  move_pct  = 100;
    if (ex_pct > 100)    ex_pct    = 100;
    if (stand_pct > 100) stand_pct = 100;

    lv_arc_set_value(c->pages[ACTIVITY_PAGE_RINGS].arcs[ACTIVITY_RING_MOVE],
                     v * move_pct / 100);
    lv_arc_set_value(c->pages[ACTIVITY_PAGE_RINGS].arcs[ACTIVITY_RING_EXERCISE],
                     v * ex_pct / 100);
    lv_arc_set_value(c->pages[ACTIVITY_PAGE_RINGS].arcs[ACTIVITY_RING_STAND],
                     v * stand_pct / 100);
}

static void activity_start_rings_anim(activity_ctx_t *c)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, c);   /* var == ctx → lv_anim_del(c, NULL) can stop it */
    lv_anim_set_exec_cb(&a, activity_rings_exec_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, ACT_RING_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ══════════════════════════════════════════════════════════════════════
 * Backlight fade (brightness driven, same look as honeycomb entry)
 * ══════════════════════════════════════════════════════════════════════ */
static void activity_fade_exec_cb(void *var, int32_t v)
{
    (void)var;
    hw_display_set_brightness((uint8_t)v);
}

static void activity_fade_in(activity_ctx_t *c)
{
    c->fade_var = 0;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &c->fade_var);
    lv_anim_set_exec_cb(&a, activity_fade_exec_cb);
    lv_anim_set_values(&a, 0, g_brightness);
    lv_anim_set_time(&a, ACT_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ══════════════════════════════════════════════════════════════════════
 * Page switching
 * ══════════════════════════════════════════════════════════════════════ */
static void activity_switch_page(activity_ctx_t *c, int new_page)
{
    if (new_page < 0)              new_page = ACTIVITY_PAGE_COUNT - 1;
    if (new_page >= ACTIVITY_PAGE_COUNT) new_page = 0;
    if (new_page == c->cur) return;

    /* Leaving the rings page → stop its count-up animation. */
    if (c->cur == ACTIVITY_PAGE_RINGS) {
        lv_anim_del(c, NULL);
    }

    c->cur = new_page;

    /* Brightness-driven fade: black out, swap, ramp back up. */
    hw_display_set_brightness(0);
    lv_screen_load(c->pages[new_page].scr);

    if (new_page == ACTIVITY_PAGE_RINGS) {
        activity_start_rings_anim(c);
    }

    activity_fade_in(c);
}

/* ══════════════════════════════════════════════════════════════════════
 * Swipe detection — manual press/release coordinates (no global LVGL
 * gesture config needed).  Attached to every interactive surface so a
 * swipe can start anywhere on the page.
 * ══════════════════════════════════════════════════════════════════════ */
static void activity_gesture_cb(lv_event_t *e)
{
    activity_ctx_t *c = (activity_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_event_get_indev(e);
        if (indev) lv_indev_get_point(indev, &c->press_start);
        c->press_cur = c->press_start;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_event_get_indev(e);
        if (indev) lv_indev_get_point(indev, &c->press_cur);
    } else if (code == LV_EVENT_RELEASED) {
        lv_point_t rel;
        lv_indev_t *indev = lv_event_get_indev(e);
        if (indev) lv_indev_get_point(indev, &rel);
        else       rel = c->press_cur;

        int32_t dy = rel.y - c->press_start.y;
        int32_t dx = rel.x - c->press_start.x;
        if (LV_ABS(dy) < ACT_SWIPE_THRESHOLD) return;
        if (LV_ABS(dy) < LV_ABS(dx) * ACT_SWIPE_RATIO) return;

        if (dy < 0) activity_switch_page(c, c->cur + 1);   /* swipe up   → next */
        else        activity_switch_page(c, c->cur - 1);   /* swipe down → prev */
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * IMU task — samples the QMI8658 at 50 Hz while the app is open and
 * derives the three activity metrics:
 *
 *   Move     — steps detected from the dynamic (gravity-free) accel
 *              magnitude; kcal ≈ steps * ACT_KCAL_PER_STEP.
 *   Exercise — minutes whose step count was brisk enough.
 *   Stand    — hours in which the wrist spent enough time "upright"
 *              (gravity mostly in the horizontal plane, i.e. |az| small).
 *
 * All thresholds are #defines — expect to tune them on the real watch.
 * The task exits itself on destroy() so it never runs after the app.
 * ══════════════════════════════════════════════════════════════════════ */
static void activity_imu_task(void *arg)
{
    activity_ctx_t *c = (activity_ctx_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / ACT_IMU_SAMPLE_HZ);

    /* Step-detector state */
    float  dyn_prev = 0.0f;          /* smoothed dynamic magnitude  */
    bool   above = false;
    int64_t last_step_ms = 0;

    /* Per-minute / per-hour accumulators */
    int   steps_this_min = 0;
    float upright_acc = 0.0f;        /* upright indicator sum this minute */
    int   upright_n = 0;
    int64_t minute_start_ms = esp_timer_get_time() / 1000;
    int64_t last_epoch_hour = minute_start_ms / 3600000;
    int   upright_min_this_hour = 0;

    while (c->imu_run) {
        hw_imu_data_t d;
        if (hw_imu_read(&d) == ESP_OK) {
            float ax = d.accel_x, ay = d.accel_y, az = d.accel_z;
            float mag = sqrtf(ax * ax + ay * ay + az * az);

            /* Dynamic component (remove gravity via slow smoothing). */
            dyn_prev = 0.85f * dyn_prev + 0.15f * mag;
            float dyn = fabsf(mag - dyn_prev);

            /* Step: rising crossing of the threshold, with a debounce. */
            if (dyn > ACT_STEP_THRESH && !above) {
                above = true;
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - last_step_ms >= ACT_STEP_DEBOUNCE_MS) {
                    last_step_ms = now_ms;
                    c->steps++;
                    steps_this_min++;
                }
            } else if (dyn < ACT_STEP_THRESH * 0.5f) {
                above = false;
            }

            /* Upright: gravity mostly horizontal → |az| well under 1 g
             * while the total stays ~1 g (not free-fall).  Which axis
             * ends up "out of the plane" depends on how the watch is
             * worn — adjust if stand never/always increments. */
            bool upright = (fabsf(az) < 0.5f * 9.81f) && (mag > 0.8f * 9.81f);
            upright_acc += upright ? 1.0f : 0.0f;
            upright_n++;
        }

        /* Minute boundary — roll the exercise + stand accumulators. */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - minute_start_ms >= 60000) {
            if (steps_this_min >= ACT_EXERCISE_STEPS_MIN) {
                if (c->exercise_min < ACT_EXERCISE_GOAL_MIN) c->exercise_min++;
            }
            steps_this_min = 0;

            if (upright_n > 0 && (upright_acc / upright_n) >= 0.5f) {
                upright_min_this_hour++;
            }
            upright_acc = 0.0f;
            upright_n = 0;
            minute_start_ms = now_ms;

            /* Hour boundary — decide whether this hour was "stood". */
            int64_t epoch_hour = now_ms / 3600000;
            if (epoch_hour != last_epoch_hour) {
                if (upright_min_this_hour >= ACT_STAND_UP_MINUTES) {
                    if (c->stand_hours < ACT_STAND_GOAL_HRS) c->stand_hours++;
                }
                upright_min_this_hour = 0;
                last_epoch_hour = epoch_hour;
            }
        }

        vTaskDelay(period);
    }

    c->imu_task = NULL;
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * Live clock — real RTC time, same 12h format as the watch face.
 * Also keeps the rolling 24 h charts aligned with the current time.
 * ══════════════════════════════════════════════════════════════════════ */
static void activity_update_time(activity_ctx_t *c)
{
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return;

    time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
    hw_rtc_time_t local;
    hw_rtc_from_unix(ts, &local);

    int h12 = local.hour % 12;
    if (h12 == 0) h12 = 12;

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", h12, local.min);

    for (int i = 0; i < ACTIVITY_PAGE_COUNT; i++) {
        lv_obj_t *lbl = c->pages[i].time;
        if (lbl && lv_obj_is_valid(lbl)) lv_label_set_text(lbl, buf);
    }

    /* Rolling chart on the three detail pages (current section last). */
    float now_frac = local.hour + local.min / 60.0f;
    for (int i = ACTIVITY_PAGE_MOVE; i < ACTIVITY_PAGE_COUNT; i++) {
        activity_pages_update_chart(&c->pages[i], now_frac);
    }
}

/* ── Live metric refresh (from the IMU task) ───────────────────────── */
static void activity_update_ui(activity_ctx_t *c)
{
    int kcal = (int)(c->steps * ACT_KCAL_PER_STEP);
    int move_pct  = (kcal * 100) / ACT_MOVE_GOAL_KCAL;
    int ex_pct    = (c->exercise_min * 100) / ACT_EXERCISE_GOAL_MIN;
    int stand_pct = (c->stand_hours * 100) / ACT_STAND_GOAL_HRS;
    if (move_pct > 100)  move_pct  = 100;
    if (ex_pct > 100)    ex_pct    = 100;
    if (stand_pct > 100) stand_pct = 100;

    /* Skip the refresh entirely when nothing changed since last tick. */
    if (kcal == c->last_kcal && c->exercise_min == c->last_ex_min &&
        c->stand_hours == c->last_stand_hours) {
        return;
    }
    c->last_kcal = kcal;
    c->last_ex_min = c->exercise_min;
    c->last_stand_hours = c->stand_hours;

    int rings[3] = { move_pct, ex_pct, stand_pct };

    /* Rings page */
    if (c->pages[ACTIVITY_PAGE_RINGS].scr) {
        for (int i = 0; i < 3; i++) {
            lv_arc_set_value(c->pages[ACTIVITY_PAGE_RINGS].arcs[i], rings[i]);
        }
    }

    /* Detail pages */
    activity_pages_set_detail_values(&c->pages[ACTIVITY_PAGE_MOVE],
                                     kcal, ACT_MOVE_GOAL_KCAL, move_pct, rings);
    activity_pages_set_detail_values(&c->pages[ACTIVITY_PAGE_EXERCISE],
                                     c->exercise_min, ACT_EXERCISE_GOAL_MIN, ex_pct, rings);
    activity_pages_set_detail_values(&c->pages[ACTIVITY_PAGE_STAND],
                                     c->stand_hours, ACT_STAND_GOAL_HRS, stand_pct, rings);
}

static void activity_clock_cb(lv_timer_t *t)
{
    activity_ctx_t *c = (activity_ctx_t *)lv_timer_get_user_data(t);
    if (!c) return;
    activity_update_time(c);
    activity_update_ui(c);
}

/* ══════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════ */
void activity_ui_create(void)
{
    if (s_ctx) {
        activity_ui_destroy();
    }

    /* App context — PSRAM */
    activity_ctx_t *c = (activity_ctx_t *)heap_caps_calloc(1, sizeof(activity_ctx_t),
                                                           MALLOC_CAP_SPIRAM);
    if (!c) {
        ESP_LOGE(TAG, "OOM: failed to allocate context in PSRAM");
        return;
    }
    s_ctx = c;

    /* Load the app's icons from SPIFFS into PSRAM before building pages. */
    activity_pages_load_icons();

    /* Build the four screens (port of my_ui, for-loop layout). */
    activity_pages_build_rings(&c->pages[ACTIVITY_PAGE_RINGS]);
    activity_pages_build_detail(ACTIVITY_PAGE_MOVE, &c->pages[ACTIVITY_PAGE_MOVE]);
    activity_pages_build_detail(ACTIVITY_PAGE_EXERCISE, &c->pages[ACTIVITY_PAGE_EXERCISE]);
    activity_pages_build_detail(ACTIVITY_PAGE_STAND, &c->pages[ACTIVITY_PAGE_STAND]);

    /* Attach the swipe handler to every interactive surface. */
    for (int i = 0; i < ACTIVITY_PAGE_COUNT; i++) {
        for (int g = 0; g < ACTIVITY_GESTURE_MAX && c->pages[i].gesture[g]; g++) {
            lv_obj_add_event_cb(c->pages[i].gesture[g], activity_gesture_cb,
                                LV_EVENT_ALL, c);
        }
    }

    /* Remember the screen to restore on exit, then show the rings page. */
    c->prev_screen = lv_screen_active();
    lv_screen_load(c->pages[ACTIVITY_PAGE_RINGS].scr);
    c->cur = ACTIVITY_PAGE_RINGS;

    /* Real-time clock + live metrics + rolling charts. */
    activity_update_time(c);
    activity_update_ui(c);
    c->timer = lv_timer_create(activity_clock_cb, 1000, c);

    /* Start the IMU sampling task (steps / exercise / stand). */
    c->imu_run = true;
    if (xTaskCreate(activity_imu_task, ACT_IMU_TASK_NAME, ACT_IMU_TASK_STACK,
                    c, ACT_IMU_TASK_PRIO, &c->imu_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IMU task");
        c->imu_run = false;
        c->imu_task = NULL;
    }

    /* Rings count up to the current values. */
    activity_start_rings_anim(c);

    ESP_LOGI(TAG, "Activity UI created (IMU task %s)",
             c->imu_task ? "started" : "failed");
}

void activity_ui_destroy(void)
{
    if (!s_ctx) return;
    activity_ctx_t *c = s_ctx;
    s_ctx = NULL;

    /* Stop the IMU task first and wait for it to exit before we free the
     * PSRAM context it shares.  It checks c->imu_run every ~20 ms. */
    c->imu_run = false;
    if (c->imu_task) {
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (c->timer) {
        lv_timer_delete(c->timer);
        c->timer = NULL;
    }

    /* Stop the ring count-up animation and any in-flight brightness fade
     * (the OS blacks the backlight and fades the menu back in itself). */
    lv_anim_del(c, NULL);
    lv_anim_del(&c->fade_var, NULL);

    /* Detach the swipe handlers first — the exit fade below keeps the old
     * screen alive for ~200 ms, so a late touch during it must not be able
     * to reach the context we are about to free. */
    for (int i = 0; i < ACTIVITY_PAGE_COUNT; i++) {
        for (int g = 0; g < ACTIVITY_GESTURE_MAX && c->pages[i].gesture[g]; g++) {
            lv_obj_remove_event_cb(c->pages[i].gesture[g], activity_gesture_cb);
        }
    }

    /* Delete every page screen except the one currently on display —
     * the fade load below auto-deletes that one after the animation. */
    lv_obj_t *active = lv_screen_active();
    for (int i = 0; i < ACTIVITY_PAGE_COUNT; i++) {
        if (c->pages[i].scr && c->pages[i].scr != active) {
            lv_obj_del(c->pages[i].scr);
        }
        c->pages[i].scr = NULL;
    }

    if (c->prev_screen && lv_obj_is_valid(c->prev_screen)) {
        /* Restore the previous screen (watch face / menu) with a fade,
         * exactly like the other apps.  auto_del=true lets LVGL free the
         * active Activity page after the animation. */
        lv_screen_load_anim(c->prev_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, true);
    } else {
        /* Defensive: no previous screen — park on a blank screen first so
         * we never leave LVGL pointing at a deleted screen. */
        lv_screen_load(lv_obj_create(NULL));
        if (active) lv_obj_del(active);
    }

    heap_caps_free(c);
    ESP_LOGI(TAG, "Activity UI destroyed (PSRAM released)");
}

bool activity_ui_is_active(void)
{
    return s_ctx != NULL;
}

void activity_ui_handle_back(void)
{
    activity_ui_destroy();
}
