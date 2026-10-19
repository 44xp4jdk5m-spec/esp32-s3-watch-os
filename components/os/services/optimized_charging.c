/**
 * @file optimized_charging.c
 * @brief Screen-usage tracking + adaptive charge current (see .h).
 */

#include "optimized_charging.h"

#include "app_config.h"
#include "app_events.h"
#include "hw_axp2101.h"
#include "hw_rtc.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "OPT_CHG";

/* ── Battery / charging constants ──────────────────────────────────── */
#define BATT_CAPACITY_MAH      400   /* cell capacity, 1 C = 400 mA */
#define MAX_CHG_MA             280   /* 0.7 C cap (25 mA steps → 275 mA) */
#define MIN_CHG_MA             25    /* AXP2101 smallest step */
#define DEFAULT_CHG_MA         400   /* normal (non-optimized) charge current */
#define FINISH_MARGIN_MIN      60    /* finish charging 1 h before morning use */
#define DEFAULT_MORNING_MIN    420   /* 07:00 fallback when no usage history */

/* ── Screen-usage history (4 weeks) ────────────────────────────────── */
#define USAGE_FILE   "/spiffs/var/lib/screen_usage.dat"
#define USAGE_DAYS   28

typedef struct {
    int64_t  day_key;         /* local-midnight timestamp used to key the day */
    int32_t  first_use_min;   /* minutes since local midnight of first active use, -1 = none */
    uint32_t active_sec;      /* total active screen seconds that day */
} screen_usage_day_t;

static screen_usage_day_t s_usage[USAGE_DAYS];
static int64_t  s_cur_day_key    = -1;   /* current (in-progress) local day */
static int32_t  s_cur_first_use  = -1;   /* first active-use minutes today */
static int64_t  s_cur_active_us  = 0;    /* active microseconds today */
static int64_t  s_last_tick_us   = 0;
static bool     s_was_active     = false;  /* display was fully on last tick */
static int64_t  s_last_day_chk   = 0;    /* last time the clock was consulted */

/* Last charge current written; starts at the init value (400 mA). */
static uint16_t s_last_chg_ma    = DEFAULT_CHG_MA;

/* ── Local clock helpers (UTC + timezone → local) ──────────────────── */
static int64_t optimized_local_day(void)
{
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return -1;
    time_t utc = hw_rtc_to_unix(&rt);
    if (utc < 1600000000) return -1;               /* clock not set (before 2020) */
    time_t local = utc + (time_t)g_config.timezone_offset * 3600;
    return (int64_t)(local - (local % 86400));     /* local midnight */
}

static int optimized_local_min(void)
{
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return -1;
    time_t utc = hw_rtc_to_unix(&rt);
    if (utc < 1600000000) return -1;
    time_t local = utc + (time_t)g_config.timezone_offset * 3600;
    return (int)((local % 86400) / 60);            /* minutes since local midnight */
}

/* ── SPIFFS persist / load ─────────────────────────────────────────── */
static void usage_save(void)
{
    FILE *f = fopen(USAGE_FILE, "w");
    if (!f) { ESP_LOGE(TAG, "Cannot open %s for write", USAGE_FILE); return; }
    fwrite(s_usage, sizeof(s_usage), 1, f);
    fclose(f);
}

static void usage_load(void)
{
    FILE *f = fopen(USAGE_FILE, "r");
    if (!f) return;                    /* no history yet */
    fread(s_usage, sizeof(s_usage), 1, f);
    fclose(f);
}

/* ── Public API ────────────────────────────────────────────────────── */
esp_err_t optimized_charging_init(void)
{
    memset(s_usage, 0, sizeof(s_usage));
    for (int i = 0; i < USAGE_DAYS; i++) s_usage[i].first_use_min = -1;
    usage_load();
    ESP_LOGI(TAG, "Optimized charging ready (4-week screen usage)");
    return ESP_OK;
}

void optimized_charging_tick(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_last_tick_us == 0) { s_last_tick_us = now_us; return; }
    int64_t delta = now_us - s_last_tick_us;
    s_last_tick_us = now_us;
    if (delta <= 0) return;

    /* "Active use" = display fully on (not dimmed / off / AOD). */
    bool active = !g_display_off && !g_display_dimmed && !g_aod_active;

    /* The main loop is BLOCKED during light sleep, so a single tick can span
     * the whole sleep (delta = many minutes).  Only add the elapsed time when
     * the display was active on BOTH this tick and the previous one — else a
     * long sleep followed by a wake-to-display-on would be wrongly counted as
     * screen-on time (the "I opened it an hour and it shows 1h" inflation). */
    if (active && s_was_active) {
        s_cur_active_us += delta;
    }
    s_was_active = active;

    /* Day rollover + first-use detection: only consult the RTC ~1/s. */
    if (now_us - s_last_day_chk < 1000000) return;
    s_last_day_chk = now_us;

    if (active && s_cur_first_use < 0) {
        s_cur_first_use = optimized_local_min();
    }

    int64_t day = optimized_local_day();
    if (day < 0) return;               /* clock invalid → skip tracking */

    if (s_cur_day_key < 0) {
        s_cur_day_key = day;           /* first valid day observed */
    } else if (day != s_cur_day_key) {
        /* Archive the finished day, drop the oldest. */
        memmove(&s_usage[1], &s_usage[0], (USAGE_DAYS - 1) * sizeof(s_usage[0]));
        s_usage[0].day_key       = s_cur_day_key;
        s_usage[0].first_use_min = s_cur_first_use;
        s_usage[0].active_sec    = (uint32_t)(s_cur_active_us / 1000000);
        s_cur_day_key    = day;
        s_cur_first_use  = -1;
        s_cur_active_us  = 0;
        usage_save();
    }
}

/* Typical first active-use minutes-of-day from the last 4 weeks.
 * Returns -1 when there is no data yet. */
static int optimized_avg_first_use_min(void)
{
    int sum = 0, count = 0;
    for (int i = 0; i < USAGE_DAYS; i++) {
        if (s_usage[i].first_use_min >= 0) {
            sum += s_usage[i].first_use_min;
            count++;
        }
    }
    if (s_cur_first_use >= 0) { sum += s_cur_first_use; count++; }
    if (count == 0) return -1;
    return sum / count;
}

void optimized_charging_update(void)
{
    /* Not in optimized mode → make sure the normal current is restored. */
    if (g_config.charge_limit_80 || !g_config.optimized_charging_enabled) {
        if (s_last_chg_ma != DEFAULT_CHG_MA) {
            if (hw_axp2101_set_charge_current(DEFAULT_CHG_MA) == ESP_OK) {
                s_last_chg_ma = DEFAULT_CHG_MA;
                ESP_LOGI(TAG, "charge current restored → %dmA", DEFAULT_CHG_MA);
            }
        }
        return;
    }

    if (!g_is_charging) return;        /* only adjust while charging */

    int now_min = optimized_local_min();
    if (now_min < 0) return;           /* clock not set → keep current */

    /* Target = typical morning use − 1 h (fallback 07:00). */
    int avg_first = optimized_avg_first_use_min();
    int morning   = (avg_first >= 0) ? avg_first : DEFAULT_MORNING_MIN;
    int target    = morning - FINISH_MARGIN_MIN;
    if (target < 0) target += 1440;

    int mins_to_target = target - now_min;
    if (mins_to_target <= 0) mins_to_target += 1440;   /* next occurrence */

    float hours = mins_to_target / 60.0f;
    if (hours < 0.05f) hours = 0.05f;

    /* 400 mAh cell → 4 mAh per %. */
    int remaining_mah = (100 - g_battery_pct) * (BATT_CAPACITY_MAH / 100);
    if (remaining_mah < 0) remaining_mah = 0;

    /* Desired current so the charge completes at the target time. */
    float desired = remaining_mah / hours;
    if (desired < MIN_CHG_MA) desired = MIN_CHG_MA;    /* never stop charging */
    if (desired > MAX_CHG_MA) desired = MAX_CHG_MA;    /* 0.7 C cap */

    uint16_t chg_ma = (uint16_t)(((int)(desired + 12.5f) / 25) * 25);  /* 25 mA steps */
    if (chg_ma < MIN_CHG_MA) chg_ma = MIN_CHG_MA;
    if (chg_ma > MAX_CHG_MA) chg_ma = MAX_CHG_MA;      /* 275 ≤ 280 */

    if (chg_ma == s_last_chg_ma) return;   /* avoid rewriting the register */

    if (hw_axp2101_set_charge_current(chg_ma) == ESP_OK) {
        s_last_chg_ma = chg_ma;
        ESP_LOGI(TAG, "charge current → %dmA (pct=%d%%, %dmAh left, %.1fh to target, morning=%02d:%02d)",
                 chg_ma, g_battery_pct, remaining_mah, hours,
                 morning / 60, morning % 60);
    }
}

/* ── Screen-time reporting (Settings → Screen Time) ────────────────── */

uint32_t optimized_charging_today_sec(void)
{
    return (uint32_t)(s_cur_active_us / 1000000);
}

uint32_t optimized_charging_last_days_sec(int n)
{
    if (n <= 0) return 0;
    if (n > USAGE_DAYS) n = USAGE_DAYS;
    uint32_t total = (uint32_t)(s_cur_active_us / 1000000);   /* today */
    for (int i = 0; i < n - 1; i++) total += s_usage[i].active_sec;
    return total;
}

uint32_t optimized_charging_total_sec(void)
{
    uint32_t total = (uint32_t)(s_cur_active_us / 1000000);   /* today */
    for (int i = 0; i < USAGE_DAYS; i++) total += s_usage[i].active_sec;
    return total;
}
