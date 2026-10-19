/**
 * @file app_time_region.c
 * @brief Time zone region & NTP server lookup table.
 *
 * Contains a static array of world regions with their UTC offsets
 * and recommended NTP pool / government time servers.
 */

#include "app_time_region.h"
#include <stddef.h>

/* ── Region data table ──────────────────────────────────────────────── */

static const time_region_t s_regions[] = {
    /* ---- Asia Pacific ---- */
    {
        .name = "Hong Kong",
        .timezone_label = "UTC+8  HKT",
        .utc_offset_hours = 8,
        .ntp_count = 2,
        .ntp_urls = { "hk.pool.ntp.org", "time.hko.gov.hk" },
    },
    {
        .name = "Taiwan",
        .timezone_label = "UTC+8  NST",
        .utc_offset_hours = 8,
        .ntp_count = 2,
        .ntp_urls = { "tw.pool.ntp.org", "time.stdtime.gov.tw" },
    },
    {
        .name = "China",
        .timezone_label = "UTC+8  CST",
        .utc_offset_hours = 8,
        .ntp_count = 1,
        .ntp_urls = { "cn.pool.ntp.org" },
    },
    {
        .name = "Japan",
        .timezone_label = "UTC+9  JST",
        .utc_offset_hours = 9,
        .ntp_count = 2,
        .ntp_urls = { "jp.pool.ntp.org", "ntp.nict.jp" },
    },
    {
        .name = "South Korea",
        .timezone_label = "UTC+9  KST",
        .utc_offset_hours = 9,
        .ntp_count = 1,
        .ntp_urls = { "kr.pool.ntp.org" },
    },
    {
        .name = "Singapore",
        .timezone_label = "UTC+8  SGT",
        .utc_offset_hours = 8,
        .ntp_count = 1,
        .ntp_urls = { "sg.pool.ntp.org" },
    },
    {
        .name = "India",
        .timezone_label = "UTC+5:30 IST",
        .utc_offset_hours = 5,
        .ntp_count = 1,
        .ntp_urls = { "in.pool.ntp.org" },
    },
    {
        .name = "Australia East",
        .timezone_label = "UTC+10 AEST",
        .utc_offset_hours = 10,
        .ntp_count = 1,
        .ntp_urls = { "au.pool.ntp.org" },
    },
    {
        .name = "New Zealand",
        .timezone_label = "UTC+12 NZST",
        .utc_offset_hours = 12,
        .ntp_count = 1,
        .ntp_urls = { "nz.pool.ntp.org" },
    },

    /* ---- Middle East ──────────────────────────────────────────────── */
    {
        .name = "UAE / Dubai",
        .timezone_label = "UTC+4  GST",
        .utc_offset_hours = 4,
        .ntp_count = 1,
        .ntp_urls = { "ae.pool.ntp.org" },
    },

    /* ---- Europe ───────────────────────────────────────────────────── */
    {
        .name = "United Kingdom",
        .timezone_label = "UTC+0  GMT",
        .utc_offset_hours = 0,
        .ntp_count = 2,
        .ntp_urls = { "uk.pool.ntp.org", "ntp.nict.uk" },
    },
    {
        .name = "Germany",
        .timezone_label = "UTC+1  CET",
        .utc_offset_hours = 1,
        .ntp_count = 1,
        .ntp_urls = { "de.pool.ntp.org" },
    },
    {
        .name = "France",
        .timezone_label = "UTC+1  CET",
        .utc_offset_hours = 1,
        .ntp_count = 1,
        .ntp_urls = { "fr.pool.ntp.org" },
    },

    /* ---- Americas ─────────────────────────────────────────────────── */
    {
        .name = "USA  Eastern",
        .timezone_label = "UTC-5  EST",
        .utc_offset_hours = -5,
        .ntp_count = 2,
        .ntp_urls = { "us.pool.ntp.org", "time.nist.gov" },
    },
    {
        .name = "USA  Central",
        .timezone_label = "UTC-6  CST",
        .utc_offset_hours = -6,
        .ntp_count = 2,
        .ntp_urls = { "us.pool.ntp.org", "time.nist.gov" },
    },
    {
        .name = "USA  Mountain",
        .timezone_label = "UTC-7  MST",
        .utc_offset_hours = -7,
        .ntp_count = 2,
        .ntp_urls = { "us.pool.ntp.org", "time.nist.gov" },
    },
    {
        .name = "USA  Pacific",
        .timezone_label = "UTC-8  PST",
        .utc_offset_hours = -8,
        .ntp_count = 2,
        .ntp_urls = { "us.pool.ntp.org", "time.nist.gov" },
    },
    {
        .name = "Brazil",
        .timezone_label = "UTC-3  BRT",
        .utc_offset_hours = -3,
        .ntp_count = 1,
        .ntp_urls = { "br.pool.ntp.org" },
    },

    /* ---- Global defaults (always at the end) ──────────────────────── */
    {
        .name = "Global",
        .timezone_label = "UTC+0  UTC",
        .utc_offset_hours = 0,
        .ntp_count = 3,
        .ntp_urls = { "pool.ntp.org", "time.google.com", "time.cloudflare.com" },
    },
};

#define REGION_COUNT  (sizeof(s_regions) / sizeof(s_regions[0]))

/* ── Public API ─────────────────────────────────────────────────────── */

uint8_t app_time_region_get_count(void)
{
    return (uint8_t)REGION_COUNT;
}

const time_region_t *app_time_region_get(uint8_t idx)
{
    if (idx >= REGION_COUNT) return NULL;
    return &s_regions[idx];
}

const char *app_time_region_get_ntp_url(uint8_t region_idx, uint8_t ntp_idx)
{
    if (region_idx >= REGION_COUNT) region_idx = 0;
    const time_region_t *r = &s_regions[region_idx];
    if (ntp_idx >= r->ntp_count) ntp_idx = 0;
    return r->ntp_urls[ntp_idx];
}

int8_t app_time_region_get_tz(uint8_t idx)
{
    if (idx >= REGION_COUNT) return 8;  /* fallback HKT */
    return s_regions[idx].utc_offset_hours;
}
