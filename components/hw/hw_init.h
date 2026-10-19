/**
 * @file hw_init.h
 * @brief Master hardware initializer and power management.
 *
 * Initializes all hardware in correct dependency order:
 *   I2C → AXP2101 PMIC → Display+Touch → Audio → IMU → RTC → Buttons
 *
 * Provides enter/exit standby to save the 400mAh battery.
 */
#pragma once

#include "hw_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize ALL hardware modules in order.
 *        Non-critical modules that fail don't block the system.
 * @param[out] result  Per-module init status
 * @return ESP_OK if critical modules (I2C, PMIC, Display) succeeded
 */
esp_err_t hw_init_all(hw_init_result_t *result);

/**
 * @brief Put all non-essential hardware into standby / low-power mode.
 *
 * What gets turned off/down:
 *   - Display brightness → 0, then backlight off
 *   - IMU → sensors disabled (accel + gyro off)
 *   - Audio → muted, codecs standby
 *   - Touch → monitor (low-power) mode
 *   - PMIC → sleep mode enabled
 *
 * What stays on:
 *   - RTC (always ultra-low-power, battery-backed)
 *   - PMIC core (DC1=3.3V)
 *   - ESP32 CPU — no light sleep (removed; light sleep caused slow/
 *     frozen UI after wake on ESP32-S3 with PSRAM + QSPI flash)
 */
esp_err_t hw_enter_standby(void);

/**
 * @brief Wake all hardware from standby mode.
 */
esp_err_t hw_exit_standby(void);

/**
 * @brief Power down only the non-display peripherals (audio codecs, SD
 *        card, IMU) — the same subset hw_enter_standby() parks — WITHOUT
 *        touching the display, touch, or PMIC.  Used to park unused
 *        hardware while the screen is still on (AOD / Low-Power idle).
 *        Idempotent; pairs with hw_unpark_peripherals().
 * @param keep_imu  true keeps the QMI8658 running because the raise-to-wake
 *                  detector / ULP sampler needs it.
 */
esp_err_t hw_park_peripherals(bool keep_imu);

/**
 * @brief Re-power the peripherals parked by hw_park_peripherals().
 *        Idempotent.
 */
esp_err_t hw_unpark_peripherals(void);

/**
 * @brief Perform a full system shutdown via PMIC.
 *        Press PWR button to restart.
 */
void hw_shutdown(void);

/**
 * @brief Poll PMIC events (call from LVGL timer or task).
 */
void hw_poll_events(void);

/**
 * @brief Print a hardware status summary to the log.
 */
void hw_dump_status(void);

#ifdef __cplusplus
}
#endif
