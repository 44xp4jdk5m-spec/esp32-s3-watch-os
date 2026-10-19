/**
 * @file app_notification_store.h
 * @brief Notification data store for the notification center.
 *
 * Holds the notifications received from iPhone (via ANCS) or pushed by
 * an on-watch app. Provides helpers for relative-time formatting and
 * app-icon resolution (matching the existing 96x96 RGB565 icon set).
 *
 * UI lives in app_notification_center.c — this module is data only.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "lvgl.h"
#include "ble_ancs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ─────────────────────────────────────────────────────────── */

#define APP_NOTIF_MAX_COUNT   8        /* how many notifications we keep */
#define APP_NOTIF_APP_ID_LEN  64
#define APP_NOTIF_TITLE_LEN   128
#define APP_NOTIF_MSG_LEN     256

/* ── Types ──────────────────────────────────────────────────────────── */

/** Where the notification came from. */
typedef enum {
    APP_NOTIF_SOURCE_PHONE = 0,   /* received from iPhone via ANCS */
    APP_NOTIF_SOURCE_APP   = 1,   /* pushed by an on-watch app */
} app_notif_source_t;

/** A single notification entry (fixed-size, no heap allocation). */
typedef struct {
    uint32_t           uid;          /* ANCS UID (or local counter for app msgs) */
    app_notif_source_t source;
    uint8_t            category_id;  /* ANCS category */
    char               app_id[APP_NOTIF_APP_ID_LEN];   /* bundle id / app key */
    char               title[APP_NOTIF_TITLE_LEN];
    char               message[APP_NOTIF_MSG_LEN];
    time_t             timestamp;    /* unix seconds (UTC) when received */
} app_notification_t;

/* ── Store API ──────────────────────────────────────────────────────── */

/**
 * @brief Add a notification received via ANCS.
 * @param n  Parsed ANCS notification (title/message populated)
 * @return index of the added/updated entry, or -1 on error
 */
int app_notification_store_add_ancs(const ancs_notification_t *n);

/**
 * @brief Add a notification pushed by an on-watch app.
 * @return index of the added entry, or -1 on error
 */
int app_notification_store_add_app(const char *app_id, const char *title, const char *msg);

/** @brief Remove the entry at @p idx (shifts later entries down). */
void app_notification_store_remove(int idx);

/** @brief Remove all entries. */
void app_notification_store_clear(void);

/** @brief Number of stored notifications (0..APP_NOTIF_MAX_COUNT). */
int app_notification_store_count(void);

/** @brief Get entry at @p idx, or NULL if out of range. */
const app_notification_t *app_notification_store_get(int idx);

/* ── Helpers ────────────────────────────────────────────────────────── */

/**
 * @brief Format a relative time string into @p buf.
 *
 * < 60s      → "now"
 * < 1h       → "5m ago"
 * < 24h      → "1h ago" / "1h 59m ago"
 * otherwise  → local date "MM/DD"
 */
void app_notification_store_format_time(time_t ts, char *buf, size_t len);

/**
 * @brief Resolve the icon for an app id (bundle id / app key).
 * @return matching lv_image_dsc_t*, or NULL if no match found.
 */
const lv_image_dsc_t *app_notification_store_resolve_icon(const char *app_id);

/** @brief Default placeholder icon used when no icon matches. */
const lv_image_dsc_t *app_notification_store_default_icon(void);

#ifdef __cplusplus
}
#endif
