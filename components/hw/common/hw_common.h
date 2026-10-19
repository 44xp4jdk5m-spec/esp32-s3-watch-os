/**
 * @file hw_common.h
 * @brief Shared types, error codes, and power mode definitions for all hardware modules.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ────────────────────────────────────────────────────────── */
typedef enum {
    HW_OK = 0,
    HW_ERR_I2C = -1,
    HW_ERR_NOT_FOUND = -2,
    HW_ERR_TIMEOUT = -3,
    HW_ERR_PARAM = -4,
    HW_ERR_NOT_INITIALIZED = -5,
    HW_ERR_ALREADY_INITIALIZED = -6,
    HW_ERR_NOT_SUPPORTED = -7,
} hw_err_t;

/* ── Power / sleep modes ──────────────────────────────────────────────── */
typedef enum {
    HW_POWER_MODE_ACTIVE = 0,
    HW_POWER_MODE_IDLE,
    HW_POWER_MODE_STANDBY,
    HW_POWER_MODE_DEEP_SLEEP,
} hw_power_mode_t;

/* ── Module initialization result ──────────────────────────────────────── */
typedef struct {
    bool i2c_ok;
    bool axp2101_ok;
    bool display_ok;
    bool touch_ok;
    bool audio_ok;
    bool imu_ok;
    bool rtc_ok;
    bool button_ok;
} hw_init_result_t;

/* ── Error-checking macros ──────────────────────────────────────────────── */
#define HW_CHECK(x) do { \
    esp_err_t __rc = (x); \
    if (__rc != ESP_OK) { \
        ESP_LOGE("HW", "Error at %s:%d -> %s = 0x%x", __FILE__, __LINE__, #x, __rc); \
        return __rc; \
    } \
} while(0)

#define HW_CHECK_GOTO(x, label) do { \
    esp_err_t __rc = (x); \
    if (__rc != ESP_OK) { \
        ESP_LOGE("HW", "Error at %s:%d -> %s = 0x%x", __FILE__, __LINE__, #x, __rc); \
        goto label; \
    } \
} while(0)

#define HW_CHECK_CONTINUE(x, tag, msg) do { \
    esp_err_t __rc = (x); \
    if (__rc != ESP_OK) { \
        ESP_LOGW(tag, "%s (err=0x%x)", msg, __rc); \
    } \
} while(0)

#ifdef __cplusplus
}
#endif
