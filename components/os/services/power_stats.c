/**
 * @file power_stats.c
 * @brief Average battery-drain estimator — see power_stats.h.
 */

#include "power_stats.h"

#include "esp_attr.h"
#include "esp_timer.h"

#define POWER_STATS_TAG    "PWR_STATS"
#define POWER_STATS_MAGIC  0x50575253u   /* "PWRS" */
#define POWER_BATT_MAH     400           /* cell capacity (matches optimized_charging) */
#define POWER_MIN_HOURS    0.05f         /* ignore windows shorter than ~3 min */

typedef struct {
    uint32_t magic;      /* POWER_STATS_MAGIC when valid */
    int32_t  peak_pct;   /* highest battery % observed (window start), -1 = none */
    int64_t  peak_us;    /* esp_timer uptime when that % was observed */
} power_stats_state_t;

/* RTC fast memory: survives light sleep and warm resets. */
static RTC_FAST_ATTR power_stats_state_t s_st;

void power_stats_init(void)
{
    int64_t now = esp_timer_get_time();
    if (s_st.magic == POWER_STATS_MAGIC && s_st.peak_us >= 0 &&
        s_st.peak_us <= now) {
        return;
    }
    s_st.magic    = POWER_STATS_MAGIC;
    s_st.peak_pct = -1;
    s_st.peak_us  = -1;
}

void power_stats_tick(int battery_pct, bool charging)
{
    if (battery_pct < 0) return;
    int64_t now = esp_timer_get_time();
    if (s_st.magic != POWER_STATS_MAGIC) power_stats_init();

    /* Uptime wrapped (warm reset) → restart the window with the current %. */
    if (s_st.peak_us < 0 || now < s_st.peak_us) {
        s_st.peak_pct = battery_pct;
        s_st.peak_us  = now;
        return;
    }

    if (charging) {
        /* Follow the charge to its peak; the discharge window then starts
         * from the top of the charge. */
        if (battery_pct > s_st.peak_pct) {
            s_st.peak_pct = battery_pct;
            s_st.peak_us  = now;
        }
    } else if (s_st.peak_pct < 0) {
        /* First observation on battery → start the window here. */
        s_st.peak_pct = battery_pct;
        s_st.peak_us  = now;
    }
}

bool power_stats_get(int current_pct, float *avg_ma, float *hours, int *peak_pct)
{
    if (avg_ma)  *avg_ma  = 0.0f;
    if (hours)   *hours   = 0.0f;
    if (peak_pct) *peak_pct = -1;
    if (s_st.magic != POWER_STATS_MAGIC || s_st.peak_pct < 0) return false;

    int64_t now = esp_timer_get_time();
    float h = (float)(now - s_st.peak_us) / 3600000000.0f;
    if (h < 0) h = 0;
    if (hours)   *hours   = h;
    if (peak_pct) *peak_pct = s_st.peak_pct;
    if (h < POWER_MIN_HOURS || current_pct < 0) return false;   /* too fresh */

    float mAh = (s_st.peak_pct - current_pct) * (POWER_BATT_MAH / 100.0f);
    if (mAh < 0) mAh = 0;
    if (avg_ma) *avg_ma = mAh / h;
    return true;
}

void power_stats_reset(void)
{
    s_st.magic    = POWER_STATS_MAGIC;
    s_st.peak_pct = -1;
    s_st.peak_us  = -1;
}
