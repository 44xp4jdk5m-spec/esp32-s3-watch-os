/**
 * @file app_temperature_monitor.c
 * @brief CPU & PMIC temperature monitor with thermal protection.
 *
 * This module runs as a low-priority FreeRTOS task, polling the
 * ESP32-S3 internal temperature sensor and the AXP2101 PMIC's
 * temperature every ~5 seconds.
 *
 * When CPU protection is enabled in settings and either temperature
 * exceeds its threshold:
 *   1. Sets g_thermal_shutdown_active = true
 *   2. Signals the UI to show a warning screen
 *   3. Stops WiFi and BLE (NOT charging — the AXP2101 throttles its own
 *      charge current in hardware when hot, and stopping charging here
 *      made the battery appear to "not charge" once the PMIC temp read
 *      real values).
 *
 * When temperatures fall back below threshold (with 2°C hysteresis):
 *   1. Re-enables WiFi / BLE as per saved config
 *   2. Signals the UI to hide the warning screen
 */

#include "app_temperature_monitor.h"
#include "app_events.h"
#include "app_config.h"
#include "app_wifi.h"
#include "app_ble_ancs.h"
#include "hw_axp2101.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "TEMP_MON";

/* ── Constants ──────────────────────────────────────────────────────── */

/** Poll interval in milliseconds. */
#define MONITOR_PERIOD_MS   5000

/** Hysteresis: temperature must drop this many °C below threshold
 *  before recovery. */
#define HYSTERESIS_C        2.0f

/** Task stack size (words). */
#define MONITOR_STACK_SIZE  4096

/** Task priority (low — don't starve LVGL). */
#define MONITOR_PRIORITY    1

/* ── ESP32-S3 internal temperature sensor ───────────────────────────── */

static temperature_sensor_handle_t s_temp_handle = NULL;

static esp_err_t cpu_temp_init(void)
{
    ESP_LOGI(TAG, "Initializing ESP32-S3 CPU temperature sensor...");

    temperature_sensor_config_t cfg = {
        .range_min = -10,
        .range_max = 80,
        .clk_src   = TEMPERATURE_SENSOR_CLK_SRC_DEFAULT,
    };

    esp_err_t ret = temperature_sensor_install(&cfg, &s_temp_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CPU temp sensor install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = temperature_sensor_enable(s_temp_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CPU temp sensor enable failed: %s", esp_err_to_name(ret));
        temperature_sensor_uninstall(s_temp_handle);
        s_temp_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "CPU temperature sensor ready");
    return ESP_OK;
}

static float cpu_temp_read(void)
{
    if (!s_temp_handle) return -1.0f;

    float c;
    esp_err_t ret = temperature_sensor_get_celsius(s_temp_handle, &c);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CPU temp read failed: %s", esp_err_to_name(ret));
        return -1.0f;
    }
    return c;
}

/* ── AXP2101 temperature ────────────────────────────────────────────── */

static float pmic_temp_read(void)
{
    float c;
    esp_err_t ret = hw_axp2101_get_temperature(&c);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PMIC temp read failed: %s", esp_err_to_name(ret));
        return -1.0f;
    }
    return c;
}

/* ── Thermal protection actions ─────────────────────────────────────── */

static void thermal_shutdown(void)
{
    if (g_thermal_shutdown_active) return;
    g_thermal_shutdown_active = true;

    ESP_LOGW(TAG, "=== THERMAL SHUTDOWN ACTIVE ===");
    ESP_LOGW(TAG, "  Stopping WiFi and BLE...");

    /* Stop WiFi */
    app_wifi_set_enabled(false);

    /* Stop BLE / ANCS */
    app_ble_ancs_stop();

    /* Do NOT stop charging — the AXP2101 regulates its own charge current
     * when hot, and force-disabling the charger here is what made the
     * battery look like it stopped charging once the PMIC temp read real
     * values (45-46°C while charging).  Battery charging keeps working. */

    /* Signal UI to show warning screen */
    g_thermal_show_warning = true;

    ESP_LOGW(TAG, "=== THERMAL SHUTDOWN COMPLETE ===");
}

static void thermal_recover(void)
{
    if (!g_thermal_shutdown_active) return;
    g_thermal_shutdown_active = false;

    ESP_LOGI(TAG, "=== THERMAL RECOVERY ===");
    ESP_LOGI(TAG, "  Temperatures back to normal, re-enabling...");

    /* Re-enable WiFi if saved config says so */
    if (g_config.wifi_enabled) {
        app_wifi_set_enabled(true);
    }

    /* Re-enable BLE if saved config says so */
    if (g_config.ble_enabled) {
        app_ble_ancs_restart();
    }

    /* Signal UI to hide warning screen */
    g_thermal_hide_warning = true;

    ESP_LOGI(TAG, "=== THERMAL RECOVERY COMPLETE ===");
}

/* ── Main monitor task ──────────────────────────────────────────────── */

static void monitor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Monitor task started (period=%dms)", MONITOR_PERIOD_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));

        /* Read temperatures */
        float cpu_temp  = cpu_temp_read();
        float pmic_temp = pmic_temp_read();

        /* Publish to globals (used by settings UI for display) */
        g_cpu_temperature  = cpu_temp;
        g_pmic_temperature = pmic_temp;

        ESP_LOGD(TAG, "Temps: CPU=%.1f°C  PMIC=%.1f°C", cpu_temp, pmic_temp);

        /* Skip if protection is disabled or sensors not ready */
        if (!g_config.cpu_protection_enabled) continue;
        if (cpu_temp < 0.0f && pmic_temp < 0.0f) continue;

        bool cpu_over  = (cpu_temp  >= 0.0f && cpu_temp  >= (float)g_config.cpu_temp_threshold);
        bool pmic_over = (pmic_temp >= 0.0f && pmic_temp >= (float)g_config.power_chips_temp_threshold);

        if (cpu_over || pmic_over) {
            ESP_LOGW(TAG, "OVER TEMP: CPU=%.1f°C(th=%d) PMIC=%.1f°C(th=%d)",
                     cpu_temp, g_config.cpu_temp_threshold,
                     pmic_temp, g_config.power_chips_temp_threshold);
            thermal_shutdown();
        } else if (g_thermal_shutdown_active) {
            /* Check for recovery with hysteresis */
            float cpu_th  = (float)g_config.cpu_temp_threshold  - HYSTERESIS_C;
            float pmic_th = (float)g_config.power_chips_temp_threshold - HYSTERESIS_C;

            bool cpu_ok  = (cpu_temp  < 0.0f || cpu_temp  <= cpu_th);
            bool pmic_ok = (pmic_temp < 0.0f || pmic_temp <= pmic_th);

            if (cpu_ok && pmic_ok) {
                ESP_LOGI(TAG, "Temps recovered: CPU=%.1f°C PMIC=%.1f°C", cpu_temp, pmic_temp);
                thermal_recover();
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t app_temperature_monitor_start(void)
{
    /* Init CPU temperature sensor */
    esp_err_t ret = cpu_temp_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CPU temp sensor failed — monitor runs with PMIC only");
    }

    BaseType_t rc = xTaskCreate(
        monitor_task,
        "temp_monitor",
        MONITOR_STACK_SIZE,
        NULL,
        MONITOR_PRIORITY,
        NULL
    );

    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Temperature monitor started");
    return ESP_OK;
}
