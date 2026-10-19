/**
 * @file app_time_region.h
 * @brief Time zone region and NTP server data for settings UI.
 *
 * Provides a lookup table of world regions with their UTC offsets
 * and recommended NTP servers. Used by the Time & Region settings
 * page to let the user choose their timezone and NTP calibration URL.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Maximums ───────────────────────────────────────────────────────── */

#define TIME_REGION_MAX_COUNT     20
#define TIME_REGION_NTP_MAX        5
#define TIME_REGION_NAME_MAX      32
#define TIME_REGION_NTP_URL_MAX   48

/* ── Region descriptor ──────────────────────────────────────────────── */

typedef struct {
    const char *name;                   /**< Display name, e.g. "Hong Kong" */
    const char *timezone_label;         /**< Short TZ label, e.g. "UTC+8 HKT" */
    int8_t      utc_offset_hours;       /**< UTC offset in hours */
    uint8_t     ntp_count;              /**< Number of NTP URLs */
    const char *ntp_urls[TIME_REGION_NTP_MAX];  /**< NTP server URLs */
} time_region_t;

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * @brief Get the number of available regions.
 */
uint8_t app_time_region_get_count(void);

/**
 * @brief Get a region by index (0 .. count-1).
 * @param idx  Region index.
 * @return Pointer to region descriptor, or NULL if out of range.
 */
const time_region_t *app_time_region_get(uint8_t idx);

/**
 * @brief Resolve the NTP URL for a given region and URL index.
 *
 * If region_idx or ntp_idx is out of range, falls back to
 * "pool.ntp.org" (region 0, URL 0).
 *
 * @param region_idx  Region index (from config).
 * @param ntp_idx     URL index within that region's list.
 * @return Pointer to a static NUL-terminated URL string.
 */
const char *app_time_region_get_ntp_url(uint8_t region_idx, uint8_t ntp_idx);

/**
 * @brief Get the timezone offset (hours) for a region index.
 * @param idx  Region index.
 * @return UTC offset in hours (falls back to 8 = HKT).
 */
int8_t app_time_region_get_tz(uint8_t idx);

#ifdef __cplusplus
}
#endif
