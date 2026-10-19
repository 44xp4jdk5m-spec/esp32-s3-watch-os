/**
 * @file esp_check.h — host stub for ESP_RETURN_ON_ERROR-style macros.
 */
#pragma once

#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...) do { \
    esp_err_t err_rc_ = (x); \
    if (err_rc_ != ESP_OK) { \
        ESP_LOGE(tag, fmt, ##__VA_ARGS__); \
        return err_rc_; \
    } \
} while (0)

#define ESP_RETURN_ON_FALSE(a, err, tag, fmt, ...) do { \
    if (!(a)) { \
        ESP_LOGE(tag, fmt, ##__VA_ARGS__); \
        return err; \
    } \
} while (0)

#define ESP_GOTO_ON_ERROR(x, goto_tag, tag, fmt, ...) do { \
    esp_err_t err_rc_ = (x); \
    if (err_rc_ != ESP_OK) { \
        ESP_LOGE(tag, fmt, ##__VA_ARGS__); \
        goto goto_tag; \
    } \
} while (0)

#define ESP_GOTO_ON_FALSE(a, err, goto_tag, tag, fmt, ...) do { \
    if (!(a)) { \
        ESP_LOGE(tag, fmt, ##__VA_ARGS__); \
        goto goto_tag; \
    } \
} while (0)
