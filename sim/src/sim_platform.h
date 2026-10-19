/**
 * @file sim_platform.h — fake sensors / board state for the simulator.
 *
 * The real firmware reads battery, charger, IMU, RTC and temperature from
 * hardware buses.  On the host those values are synthesised so the UI has
 * something plausible to display.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Advance the fake sensors by @p dt_ms milliseconds. */
void sim_platform_tick(uint32_t dt_ms);

/* ── Simulated state ──────────────────────────────────────────────── */
int  sim_battery_pct(void);            /**< 0..100 */
bool sim_battery_charging(void);
float sim_battery_voltage(void);       /**< volts */
float sim_chip_temp_c(void);
float sim_battery_temp_c(void);

/** Tilt in degrees, driven by the mouse when dragging. */
void sim_set_tilt(float x_deg, float y_deg);
void sim_get_tilt(float *x_deg, float *y_deg);

/** Accelerometer (g) with gravity + a little noise. */
void sim_get_accel(float *x, float *y, float *z);

/** Simulated wrist motion, used by wake-on-raise. */
void sim_raise_wrist(void);

/** @return true while a simulated raise-to-wake gesture is active. */
bool sim_raise_wrist_active(void);

/** @return true if the clock should follow the host wall clock. */
bool sim_use_real_time(void);

/** Run any expired esp_timers (called by the main loop). */
void sim_platform_run_timers(void);

/** Toggle wall-clock: real host time (true) or a fixed demo time (false). */
void sim_set_use_real_time(bool real);

#ifdef __cplusplus
}
#endif
