/**
 * @file battery_history.h
 * @brief Hourly battery history storage (Settings → Power → Battery graph).
 *
 * Lives in the OS layer so both the Settings UI and the light-sleep
 * housekeeping timer path can record a sample without an apps→os
 * dependency.  A sample is normally taken once per hour, aligned to the
 * local wall-clock hour (:00) — see battery_history_poll() (main loop,
 * display on) and battery_history_next_wake_us() (light-sleep timer, so
 * the CPU wakes at the next :00 even when the display is off).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of hourly samples kept (24 = last 24 h, 1 bar/hour). */
#define BATTERY_HISTORY_MAX  24

/** One hourly battery record. */
typedef struct {
    uint8_t  pct;         /**< Battery percentage 0–100 */
    bool     is_charging; /**< Was charging during this hour */
} battery_history_entry_t;

/**
 * @brief Load battery history from SPIFFS.
 * @param entries  output array (must be BATTERY_HISTORY_MAX in size)
 * @return number of valid entries loaded (0 if no data)
 */
int battery_history_load(battery_history_entry_t *entries);

/**
 * @brief Save battery history to SPIFFS.
 * @param entries array of BATTERY_HISTORY_MAX entries
 */
void battery_history_save(const battery_history_entry_t *entries);

/**
 * @brief Record the current battery state into the history ring buffer
 *        and persist to SPIFFS.  Marks the current local hour as recorded
 *        so a later poll in the same hour is a no-op.
 */
void battery_history_record(void);

/**
 * @brief Main-loop helper (call every iteration — internally throttled).
 *
 * Records a sample when the local wall-clock hour advances past the last
 * recorded hour (so an hourly, :00-aligned series is kept while the CPU is
 * awake).  Also supplies the quick first sample after boot.
 */
void battery_history_poll(void);

/**
 * @brief Microseconds until the next local wall-clock hour boundary (:00).
 *
 * Used by the power manager to arm a one-shot light-sleep timer so the
 * watch wakes at the top of the hour and records a battery sample even
 * while the display is off.  Returns 0 when the RTC is unavailable/not
 * set (no timer should then be armed).
 */
uint64_t battery_history_next_wake_us(void);

#ifdef __cplusplus
}
#endif
