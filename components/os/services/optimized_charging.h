/**
 * @file optimized_charging.h
 * @brief Slow "finish-by-morning" adaptive charging.
 *
 * When the 80% charge limit is OFF and "Optimized Charging" is enabled:
 *  - The watch records how long the screen is ACTIVELY used each day
 *    (full brightness — not dimmed / off / AOD) and persists 4 weeks of
 *    daily records to SPIFFS.
 *  - From that history it derives the user's typical first morning use.
 *  - While charging it sets the AXP2101 charge current so the battery
 *    finishes ~1 h before that time — as slow as possible (down to the
 *    25 mA minimum, charging is never stopped) and never faster than
 *    0.7 C (280 mA, i.e. 275 mA in 25 mA steps).
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Load the 4-week screen-usage history from SPIFFS. Call once at boot. */
esp_err_t optimized_charging_init(void);

/** @brief Accumulate active screen time. Call every main-loop iteration
 *         (cheap; only touches I2C/RTC about once per second). */
void optimized_charging_tick(void);

/** @brief Recompute/set the AXP2101 charge current. Call from the battery
 *         poll (every ~500 ms). Also restores the normal 400 mA current
 *         whenever optimized charging is not active. */
void optimized_charging_update(void);

/* ── Screen-time reporting (Settings → Screen Time) ───────────────── */

/** @brief Active screen seconds so far today (in-progress day). */
uint32_t optimized_charging_today_sec(void);

/** @brief Active screen seconds over the last `n` days including today
 *         (today + up to n-1 completed days from the 4-week history). */
uint32_t optimized_charging_last_days_sec(int n);

/** @brief Total active screen seconds in the whole 4-week history. */
uint32_t optimized_charging_total_sec(void);

#ifdef __cplusplus
}
#endif
