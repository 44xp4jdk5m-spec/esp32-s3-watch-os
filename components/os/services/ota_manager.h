/**
 * @file ota_manager.h
 * @brief OTA over WiFi: download firmware to the OTA partition, then apply.
 *
 * Flow:
 *   1. ota_manager_start_download() — asynchronously downloads the .bin
 *      from g_config.ota_url into the next OTA partition (esp_ota_write),
 *      reporting progress.  On success the firmware is "staged".
 *   2. ota_manager_install() — shows a full-screen install UI reusing the
 *      shared radial progress ring, closes services (WiFi file server,
 *      BLE, SD card) to avoid data loss, sets the boot partition, then
 *      reboots into the new firmware.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_IDLE = 0,
    OTA_DOWNLOADING,
    OTA_DOWNLOAD_DONE,      /* firmware staged, ready to install */
    OTA_DOWNLOAD_FAILED,
    OTA_INSTALLING,
} ota_state_t;

/** Initialise the OTA manager (call once at boot). */
void ota_manager_init(void);

/** Current OTA state machine state. */
ota_state_t ota_manager_get_state(void);

/** Download/install progress 0..100. */
int ota_manager_get_progress(void);

/** Start an async firmware download from g_config.ota_url into the next
 *  OTA partition.  Returns false if disabled / no URL / already running. */
bool ota_manager_start_download(void);

/** Apply the staged firmware: close services, set boot partition, reboot.
 *  No-op unless the download has completed. */
void ota_manager_install(void);

#ifdef __cplusplus
}
#endif
