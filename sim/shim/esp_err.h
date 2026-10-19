/**
 * @file esp_err.h — host stub for the ESP-IDF error-code header.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef int esp_err_t;

#define ESP_OK          0
#define ESP_FAIL        (-1)

#define ESP_ERR_NO_MEM              0x101
#define ESP_ERR_INVALID_ARG         0x102
#define ESP_ERR_INVALID_STATE       0x103
#define ESP_ERR_INVALID_SIZE        0x104
#define ESP_ERR_NOT_FOUND           0x105
#define ESP_ERR_NOT_SUPPORTED       0x106
#define ESP_ERR_TIMEOUT             0x107
#define ESP_ERR_INVALID_RESPONSE    0x108
#define ESP_ERR_INVALID_CRC         0x109
#define ESP_ERR_INVALID_VERSION     0x10A
#define ESP_ERR_INVALID_MAC         0x10B
#define ESP_ERR_NOT_FINISHED        0x10C
#define ESP_ERR_NOT_ENOUGH_UNUSED_PAGES 0x10D
#define ESP_ERR_INVALID_HANDLE      0x10E
#define ESP_ERR_NVS_BASE            0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NO_FREE_PAGES   (ESP_ERR_NVS_BASE + 0x0D)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)

const char *esp_err_to_name(esp_err_t code);
const char *esp_err_to_name_r(esp_err_t code, char *buf, size_t buflen);

#define ESP_ERROR_CHECK(x) do { esp_err_t __e = (x); (void)__e; } while (0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ESP_ERROR_CHECK(x)
