/**
 * @file tinyusb_msc.h
 * @brief TinyUSB Mass Storage Class — expose SD card as USB drive.
 *
 * Uses USB OTG peripheral (GPIO19 D-, GPIO20 D+). USB Serial/JTAG
 * secondary console must be disabled in menuconfig to free the lines.
 */

#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tinyusb_msc_init(void);
bool tinyusb_msc_is_active(void);
void tinyusb_msc_deinit(void);

#ifdef __cplusplus
}
#endif
