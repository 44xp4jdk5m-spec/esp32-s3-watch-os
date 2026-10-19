/**
 * @file app_usage.c
 * @brief Per-app screen-time usage tracker (see app_usage.h).
 *
 * Design notes:
 *  - One "bucket" is the foreground while the display is fully on.  It is
 *    either an installed app (resolved to an app_registry index → a
 *    category) or the system aggregate (CAT_SYSTEM).  Every active second
 *    lands in exactly one bucket, so summing the categories of a day equals
 *    that day's active screen-on time.
 *  - Live seconds are accumulated in microseconds (today only); the ring of
 *    trailing days stores whole seconds per category and is snapshotted to
 *    SPIFFS whenever the user stops using the watch (display leaves active),
 *    on a day rollover, and at init.
 *  - Per-app data is TODAY only (cleared at the local-day rollover) because
 *    the Screen Time page only lists per-app usage for today.
 */

#include "app_usage.h"
#include "app_config.h"
#include "app_events.h"
#include "app_registry.h"
#include "hw_rtc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_USAGE";

#define USAGE_FILE   "/spiffs/var/lib/app_usage.dat"
#define USAGE_RING   10            /* stored trailing days (>= TRACK_DAYS) */

/* Per-day record (whole seconds per category). */
typedef struct {
    int64_t  day_key;              /* local midnight of that day */
    uint32_t cat_sec[APP_USAGE_CAT_N];
} day_rec_t;

static day_rec_t s_ring[USAGE_RING];  /* [0..s_valid-1], newest = last */
static int       s_valid = 0;

/* Live "today" state. */
static int64_t   s_today_key = -1;
static uint64_t  s_today_us[APP_USAGE_CAT_N];

/* Today per-app (by app_registry index). */
#define MAX_APP_SLOTS 64
static int      s_app_idx[MAX_APP_SLOTS];
static uint64_t s_app_us[MAX_APP_SLOTS];
static int      s_app_count = 0;

/* Tick state. */
static int64_t  s_last_tick_us = 0;
static bool     s_was_active   = false;
static int64_t  s_last_day_chk = 0;

/* Current bucket cache. */
static app_screen_t s_res_screen = (app_screen_t)-1;
static int          s_res_js     = -99;   /* last JS index seen */
static int          s_res_index  = -1;    /* resolved registry index */

/* JS running-app provider (registered by the shell). */
static int (*s_js_active_fn)(void) = NULL;

void app_usage_set_js_active_fn(int (*fn)(void)) { s_js_active_fn = fn; }

/* ── Local clock ───────────────────────────────────────────────────── */
static int64_t app_usage_local_day(void)
{
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return -1;
    time_t utc = hw_rtc_to_unix(&rt);
    if (utc < 1600000000) return -1;
    time_t local = utc + (time_t)g_config.timezone_offset * 3600;
    return (int64_t)(local - (local % 86400));
}

/* ── Category mapping (default; no per-app metadata yet) ───────────── */
static app_usage_cat_t app_usage_cat_of_index(int idx)
{
    const app_entry_t *e = app_registry_get(idx);
    if (!e || !e->id) return APP_USAGE_CAT_OTHER;
    if (e->is_js) return APP_USAGE_CAT_OTHER;
    if (strcmp(e->id, "settings") == 0)   return APP_USAGE_CAT_SYSTEM;
    if (strcmp(e->id, "calculator") == 0) return APP_USAGE_CAT_PRODUCTIVITY;
    return APP_USAGE_CAT_OTHER;   /* photos / noise / activity + future */
}

static int app_usage_find_id(const char *id)
{
    int n = app_registry_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *e = app_registry_get(i);
        if (e && e->id && strcmp(e->id, id) == 0) return i;
    }
    return -1;
}

static int app_usage_find_js(int js)
{
    int n = app_registry_count();
    for (int i = 0; i < n; i++) {
        const app_entry_t *e = app_registry_get(i);
        if (e && e->is_js && e->js_index == js) return i;
    }
    return -1;
}

/* Resolve the current foreground bucket. */
static void app_usage_bucket(int *out_idx, app_usage_cat_t *out_cat)
{
    int js = (s_js_active_fn) ? s_js_active_fn() : -1;

    if (g_current_screen != s_res_screen || js != s_res_js) {
        int idx = -1;
        switch (g_current_screen) {
        case APP_SCREEN_SETTINGS:   idx = app_usage_find_id("settings");   break;
        case APP_SCREEN_PHOTOS:     idx = app_usage_find_id("photos");     break;
        case APP_SCREEN_CALCULATOR: idx = app_usage_find_id("calculator"); break;
        case APP_SCREEN_NOISE:      idx = app_usage_find_id("noise");      break;
        case APP_SCREEN_ACTIVITY:   idx = app_usage_find_id("activity");   break;
        case APP_SCREEN_JSAPPS:
            if (js >= 0) idx = app_usage_find_js(js);   /* running JS app */
            break;
        default: break;             /* watch face / menus / CC / … = system */
        }
        s_res_index = idx;
        s_res_screen = g_current_screen;
        s_res_js = js;
    }

    *out_idx = s_res_index;
    *out_cat = (s_res_index >= 0) ? app_usage_cat_of_index(s_res_index)
                                  : APP_USAGE_CAT_SYSTEM;
}

/* ── Today per-app slot helpers ────────────────────────────────────── */
static void app_usage_app_add(int idx, uint64_t us)
{
    if (idx < 0) return;
    for (int i = 0; i < s_app_count; i++) {
        if (s_app_idx[i] == idx) { s_app_us[i] += us; return; }
    }
    if (s_app_count < MAX_APP_SLOTS) {
        s_app_idx[s_app_count] = idx;
        s_app_us[s_app_count]  = us;
        s_app_count++;
    }
}

/* ── Persist ───────────────────────────────────────────────────────── */
static void app_usage_sync_ring_today(void)
{
    if (s_today_key < 0) return;
    if (s_valid > 0 && s_ring[s_valid - 1].day_key == s_today_key) {
        for (int c = 0; c < APP_USAGE_CAT_N; c++)
            s_ring[s_valid - 1].cat_sec[c] = (uint32_t)(s_today_us[c] / 1000000ULL);
        return;
    }
    if (s_valid >= USAGE_RING) {
        memmove(&s_ring[0], &s_ring[1], (USAGE_RING - 1) * sizeof(day_rec_t));
        s_valid = USAGE_RING - 1;
    }
    day_rec_t *d = &s_ring[s_valid++];
    memset(d, 0, sizeof(*d));
    d->day_key = s_today_key;
    for (int c = 0; c < APP_USAGE_CAT_N; c++)
        d->cat_sec[c] = (uint32_t)(s_today_us[c] / 1000000ULL);
}

static void app_usage_save(void)
{
    app_usage_sync_ring_today();
    FILE *f = fopen(USAGE_FILE, "w");
    if (!f) { ESP_LOGE(TAG, "Cannot open %s for write", USAGE_FILE); return; }
    fwrite(&s_valid, sizeof(s_valid), 1, f);
    fwrite(s_ring, sizeof(day_rec_t), (size_t)s_valid, f);
    fclose(f);
}

/* ── Public API ────────────────────────────────────────────────────── */
esp_err_t app_usage_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_valid = 0;
    FILE *f = fopen(USAGE_FILE, "r");
    if (f) {
        int n = 0;
        if (fread(&n, sizeof(n), 1, f) == 1 && n > 0 && n <= USAGE_RING) {
            if (fread(s_ring, sizeof(day_rec_t), (size_t)n, f) == (size_t)n) {
                s_valid = n;
            }
        }
        fclose(f);
    }
    ESP_LOGI(TAG, "App usage ready (%d stored days)", s_valid);
    return ESP_OK;
}

/* Handle (possible) day rollover / first valid RTC day. */
static void app_usage_day_update(int64_t day)
{
    if (s_today_key == day) return;

    if (s_today_key < 0) {
        /* Startup / first valid RTC day. */
        s_today_key = day;
        /* Same-day reboot: adopt the partially persisted "today". */
        if (s_valid > 0 && s_ring[s_valid - 1].day_key == day) {
            for (int c = 0; c < APP_USAGE_CAT_N; c++)
                s_today_us[c] = (uint64_t)s_ring[s_valid - 1].cat_sec[c] * 1000000ULL;
        }
    } else {
        /* The previous day finished: finalize it in the ring. */
        app_usage_sync_ring_today();
        s_today_key = day;
        memset(s_today_us, 0, sizeof(s_today_us));
        s_app_count = 0;
    }
    app_usage_save();
}

void app_usage_tick(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_last_tick_us == 0) { s_last_tick_us = now_us; return; }
    int64_t delta = now_us - s_last_tick_us;
    s_last_tick_us = now_us;
    if (delta <= 0) return;

    /* "Active" = display fully on (not dimmed / off / AOD). */
    bool active = !g_display_off && !g_display_dimmed && !g_aod_active;

    /* When the display leaves active (user stopped), snapshot to SPIFFS. */
    if (!active && s_was_active) app_usage_save();

    if (active && s_was_active) {
        /* Only consult the clock ~1/s for rollover. */
        if (now_us - s_last_day_chk >= 1000000) {
            s_last_day_chk = now_us;
            int64_t day = app_usage_local_day();
            if (day >= 0) app_usage_day_update(day);
        }
        if (s_today_key >= 0) {
            int idx;
            app_usage_cat_t cat;
            app_usage_bucket(&idx, &cat);
            s_today_us[cat] += (uint64_t)delta;
            if (idx >= 0) app_usage_app_add(idx, (uint64_t)delta);
        }
    }
    s_was_active = active;
}

/* ── UI getters ────────────────────────────────────────────────────── */
uint32_t app_usage_cat_sec(int day_ago, app_usage_cat_t cat)
{
    if (cat < 0 || cat >= APP_USAGE_CAT_N) return 0;
    if (day_ago < 0 || day_ago >= APP_USAGE_TRACK_DAYS) return 0;

    if (day_ago == 0 && s_today_key >= 0)
        return (uint32_t)(s_today_us[cat] / 1000000ULL);

    int64_t key = (s_today_key >= 0)
                  ? s_today_key - (int64_t)day_ago * 86400 : 0;
    for (int i = s_valid - 1; i >= 0; i--) {
        if (s_ring[i].day_key == key) return s_ring[i].cat_sec[cat];
    }
    return 0;
}

uint32_t app_usage_total_sec(int day_ago)
{
    uint32_t sum = 0;
    for (int c = 0; c < APP_USAGE_CAT_N; c++)
        sum += app_usage_cat_sec(day_ago, (app_usage_cat_t)c);
    return sum;
}

uint32_t app_usage_week_total_sec(void)
{
    uint32_t sum = 0;
    for (int d = 0; d < APP_USAGE_TRACK_DAYS; d++) sum += app_usage_total_sec(d);
    return sum;
}

uint32_t app_usage_app_sec(int reg_index)
{
    for (int i = 0; i < s_app_count; i++) {
        if (s_app_idx[i] == reg_index) return (uint32_t)(s_app_us[i] / 1000000ULL);
    }
    return 0;
}
