/**
 * @file battery_history.c
 * @brief Hourly battery history (see battery_history.h).
 *
 * Moved here from the Settings app so the OS power/sleep layer can record
 * samples without depending on apps.  The ring stores 24 hourly samples;
 * recording is aligned to the local wall-clock hour:
 *   - battery_history_poll()   — called from the main loop (display on),
 *                                records when the hour advances.
 *   - battery_history_record() — called directly by the light-sleep
 *                                housekeeping TIMER wake at each :00.
 */

#include "battery_history.h"
#include "app_config.h"
#include "app_events.h"
#include "hw_axp2101.h"
#include "hw_rtc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "BATT_HIST";

#define BATT_HIST_FILE  "/spiffs/var/lib/battery_history.dat"

/* Local hour of the last recorded sample (-1 = none yet, e.g. first boot).
 * RAM only: it survives light sleep (retained) and resets on a power cycle
 * which is fine because battery_history_poll() re-seeds it quickly. */
static int8_t   s_last_hour   = -1;
static int64_t  s_last_poll_us = 0;

/* ── Local clock helper (UTC + timezone → local hour) ──────────────── */
static int battery_local_hour(void)
{
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return -1;
    time_t utc = hw_rtc_to_unix(&rt);
    if (utc < 1600000000) return -1;            /* clock not set (before 2020) */
    time_t local = utc + (time_t)g_config.timezone_offset * 3600;
    int64_t h = (local % 86400) / 3600;
    if (h < 0) h += 24;
    return (int)h;
}

/* ── SPIFFS load / save ────────────────────────────────────────────── */
int battery_history_load(battery_history_entry_t *entries)
{
    FILE *f = fopen(BATT_HIST_FILE, "r");
    if (!f) {
        ESP_LOGW(TAG, "No battery history file found");
        return 0;
    }
    size_t read = fread(entries, sizeof(battery_history_entry_t),
                        BATTERY_HISTORY_MAX, f);
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d battery history entries", (int)read);
    return (int)read;
}

void battery_history_save(const battery_history_entry_t *entries)
{
    FILE *f = fopen(BATT_HIST_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing", BATT_HIST_FILE);
        return;
    }
    fwrite(entries, sizeof(battery_history_entry_t),
           BATTERY_HISTORY_MAX, f);
    fclose(f);
    ESP_LOGI(TAG, "Battery history saved (%d entries)", BATTERY_HISTORY_MAX);
}

/* ── Recording ─────────────────────────────────────────────────────── */
void battery_history_record(void)
{
    int hour = battery_local_hour();
    if (hour >= 0) s_last_hour = (int8_t)hour;

    /* Read a fresh PMIC battery level — when this runs from a light-sleep
     * housekeeping wake the globals may be stale (up to an hour old). */
    int  pct = g_battery_pct;
    bool chg = g_is_charging;
    if (hw_axp2101_get_battery_percent(&pct) == ESP_OK) g_battery_pct = pct;
    if (hw_axp2101_is_charging(&chg) == ESP_OK)         g_is_charging = chg;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    battery_history_entry_t hist[BATTERY_HISTORY_MAX] = {0};
    int count = battery_history_load(hist);

    battery_history_entry_t new_hist[BATTERY_HISTORY_MAX] = {0};
    int copy_count = count;
    if (copy_count >= BATTERY_HISTORY_MAX) copy_count = BATTERY_HISTORY_MAX - 1;

    for (int i = 0; i < copy_count; i++) {
        new_hist[i] = hist[count - copy_count + i];
    }

    new_hist[copy_count].pct = (uint8_t)pct;
    new_hist[copy_count].is_charging = chg;

    if (pct >= 100 && !chg) {
        hw_rtc_time_t rt;
        if (hw_rtc_get_time(&rt) == ESP_OK) {
            g_config.last_charged_to_100_ts = hw_rtc_to_unix(&rt);
            app_config_save();
        }
    }

    battery_history_save(new_hist);
}

/* Main-loop poll: record once per local hour.  Throttled to ~5 s so we do
 * not hit the RTC over I2C on every loop iteration. */
void battery_history_poll(void)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_poll_us < 5000000LL) return;
    s_last_poll_us = now_us;

    int hour = battery_local_hour();
    if (hour < 0) return;                        /* RTC not set yet */
    if (hour == (int)s_last_hour) return;
    battery_history_record();                    /* sets s_last_hour */
}

/* Microseconds until the next local hour boundary (:00).  Returns 0 when
 * the RTC is unavailable so callers can skip arming a sleep timer. */
uint64_t battery_history_next_wake_us(void)
{
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return 0;
    time_t utc = hw_rtc_to_unix(&rt);
    if (utc < 1600000000) return 0;

    time_t local = utc + (time_t)g_config.timezone_offset * 3600;
    time_t next_local_hour = ((local / 3600) + 1) * 3600;
    time_t next_utc = next_local_hour - (time_t)g_config.timezone_offset * 3600;

    int64_t diff_us = ((int64_t)(next_utc - utc)) * 1000000LL;
    if (diff_us <= 0) diff_us = 3600LL * 1000000LL;   /* safety fallback */
    if (diff_us > 3600LL * 1000000LL) diff_us = 3600LL * 1000000LL;
    return (uint64_t)diff_us;
}
