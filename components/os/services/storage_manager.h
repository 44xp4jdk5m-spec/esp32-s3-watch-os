/**
 * @file storage_manager.h
 * @brief Central orchestrator for screenshot → upload → cache workflow.
 *
 * Coordinates:
 *   1. Screen capture (LVGL snapshot → PSRAM)
 *   2. BMP encoding in PSRAM
 *   3. Google Drive upload (when WiFi available)
 *   4. SD card cache fallback (when no WiFi)
 *   5. Pending upload queue processing when WiFi reconnects
 *   6. On-screen upload notice
 *
 * All large allocations use PSRAM (MALLOC_CAP_SPIRAM).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the storage manager.
 *
 * Mounts the SD card cache (if available) and loads config.
 * Must be called once during boot.
 */
void storage_manager_init(void);

/**
 * @brief Trigger a screenshot capture → encode → upload sequence.
 *
 * This is the main entry point called when GPIO10+GPIO0 combo is detected.
 * It runs the full pipeline:
 *   1. Capture LVGL screen to PSRAM
 *   2. Encode as BMP in PSRAM
 *   3. If WiFi connected → upload to Google Drive → show notice
 *   4. If WiFi not connected → save to SD cache → show notice
 *   5. Clean up PSRAM buffers
 *
 * This function is async-safe and spawns a FreeRTOS task for the
 * heavy work (HTTP, file I/O) to avoid blocking the main loop.
 */
void storage_manager_trigger_screenshot(void);

/**
 * @brief Process pending uploads from SD card cache.
 *
 * Called periodically (or when WiFi connects) to upload any cached
 * screenshots that were saved while offline.
 *
 * Will process up to `max_count` files per call, then yield.
 */
void storage_manager_process_pending_uploads(uint32_t max_count);

/**
 * @brief Get the number of cached screenshots pending upload.
 */
uint32_t storage_manager_pending_count(void);

/**
 * @brief Get the number of screenshots captured this session.
 */
uint32_t storage_manager_capture_count(void);

/**
 * @brief Generate a timestamp-based filename.
 *
 * Format: "YYYY-MM-DD_HH-MM-SS.bmp"
 * Uses RTC time (UTC → local via config timezone offset).
 *
 * @param buf   Output buffer (must be at least 32 bytes).
 * @param size  Buffer size.
 * @return Pointer to buf, or NULL if RTC read failed.
 */
char *storage_manager_make_filename(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
