/**
 * @file power_stats.h
 * @brief Average battery-drain estimator (Settings → Screen Time).
 *
 * The AXP2101 PMIC has NO current/ADC channel (its ADC only measures
 * voltages + temperature, see datasheet Table 6-7), so we cannot read an
 * instantaneous current.  Instead we estimate the average battery discharge
 * from the fuel-gauge percentage slope:
 *
 *   avg_mA = (peak_pct - current_pct) * capacity_mAh / 100 / hours
 *
 * The window starts at the highest charge % observed (set while charging)
 * or at the first unplugged observation, and is kept in RTC fast memory so
 * it survives light sleep and warm resets (a reset just restarts the
 * window, which is what you want after flashing a new build).
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialise the estimator (safe to call repeatedly). */
void power_stats_init(void);

/**
 * @brief Feed the current battery state (call ~1/s from the main loop).
 *
 * @param battery_pct  fuel-gauge % (0-100), -1 if unknown.
 * @param charging     true while a charger is connected.
 */
void power_stats_tick(int battery_pct, bool charging);

/**
 * @brief Get the average discharge since the measurement window started.
 *
 * @param[in]  current_pct  current fuel-gauge % (used for the delta).
 * @param[out] avg_ma       average discharge current (mA) if measurable.
 * @param[out] hours        elapsed hours in the window (0 if just started).
 * @param[out] peak_pct     battery % at the window start.
 * @return true if a window is active and long enough to be meaningful
 *         (> ~3 min); false otherwise (avg_ma is 0 then).
 */
bool power_stats_get(int current_pct, float *avg_ma, float *hours, int *peak_pct);

/** @brief Reset the measurement window (start fresh). */
void power_stats_reset(void);

#ifdef __cplusplus
}
#endif
