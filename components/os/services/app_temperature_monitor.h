/**
 * @file app_temperature_monitor.h
 * @brief CPU & PMIC temperature monitor with thermal protection.
 *
 * Periodically reads ESP32-S3 internal temperature sensor and AXP2101
 * PMIC temperature.  If either exceeds the configured threshold while
 * CPU protection is enabled, this module stops WiFi, BLE, and charging
 * until temperatures return to safe levels.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the temperature monitor FreeRTOS task.
 *
 * Must be called once after hw_init_all() and app_config_init().
 *
 * @return ESP_OK or error.
 */
esp_err_t app_temperature_monitor_start(void);

#ifdef __cplusplus
}
#endif
