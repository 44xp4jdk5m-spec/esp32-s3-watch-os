/**
 * @file app_usage.h
 * @brief Per-app screen-time usage tracker (Settings → Power → Screen Time).
 *
 * Attributes every fully-on "active" screen second to a usage bucket:
 *   - a native/JS app (by app_registry index) → its category,
 *   - or the system aggregate (watch face, launcher, menus, control
 *     center, charging screen, …) → CAT_SYSTEM.
 *
 * Daily totals are stored per category so the Screen Time page can draw one
 * stacked bar per calendar day; per-app data is kept for TODAY only (the
 * per-app list on the page shows today's usage).  A few trailing days are
 * persisted to SPIFFS so history survives reboots.
 *
 * "Active" matches the OS screen-time metric: display fully on, not
 * dimmed / off / AOD (same window as optimized_charging).
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** App categories (colour order matches the stacked graph, bottom→top). */
typedef enum {
    APP_USAGE_CAT_SOCIAL       = 0,   /* blue      */
    APP_USAGE_CAT_PRODUCTIVITY = 1,   /* light blue */
    APP_USAGE_CAT_OTHER        = 2,   /* orange    */
    APP_USAGE_CAT_SYSTEM       = 3,   /* gray      */
    APP_USAGE_CAT_N            = 4
} app_usage_cat_t;

/** Number of trailing calendar days the Screen Time graph shows. */
#define APP_USAGE_TRACK_DAYS   7

/**
 * @brief Load persisted per-day history.  Call once at boot.
 */
esp_err_t app_usage_init(void);

/**
 * @brief Accumulate screen time into the current bucket.  Call every
 *        main-loop iteration (cheap; RTC only consulted ~1/s).
 */
void app_usage_tick(void);

/**
 * @brief Register a callback returning the currently running JS app index
 *        (-1 = launcher / none).  The os layer cannot see the JS runtime
 *        directly, so the shell registers jsapp_run_active_index().
 */
void app_usage_set_js_active_fn(int (*fn)(void));

/* ── UI getters ────────────────────────────────────────────────────── */

/**
 * @brief Category seconds for a trailing calendar day.
 * @param day_ago 0 = today, 1 = yesterday … up to APP_USAGE_TRACK_DAYS-1.
 */
uint32_t app_usage_cat_sec(int day_ago, app_usage_cat_t cat);

/** @brief Total active seconds for a trailing calendar day. */
uint32_t app_usage_total_sec(int day_ago);

/** @brief Total active seconds over the last APP_USAGE_TRACK_DAYS days
 *         including today. */
uint32_t app_usage_week_total_sec(void);

/** @brief TODAY's active seconds for one installed app (registry index). */
uint32_t app_usage_app_sec(int reg_index);

#ifdef __cplusplus
}
#endif
