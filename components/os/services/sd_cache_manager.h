/**
 * @file sd_cache_manager.h
 * @brief SD card cache for offline screenshot queue.
 *
 * When WiFi is unavailable, screenshots are saved to the SD card in a
 * "screenshot_cache" directory.  A queue file tracks which screenshots
 * still need to be uploaded.  When WiFi becomes available, the pending
 * uploads are processed in FIFO order.
 *
 * Queue is stored as a simple CSV file on the SD card:
 *   /shots/queue.csv
 * Each line:  filename,size_bytes,timestamp_epoch
 *
 * All file I/O buffers use PSRAM where possible.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Max number of pending screenshots in the queue. */
#define SD_CACHE_MAX_PENDING    128

/** Max filename length (inc. null). */
#define SD_CACHE_FILENAME_MAX   64

/** Full path to the cache directory on the SD card.
 *  Must be a short 8.3 name (FAT long filenames disabled in menuconfig). */
#define SD_CACHE_DIR            "/sdcard/shots"
#define SD_CACHE_QUEUE_FILE     SD_CACHE_DIR "/queue.csv"

/**
 * @brief Initialize the SD card cache.
 *
 * Creates the cache directory and queue file if they don't exist.
 * Must be called before any other sd_cache_* functions.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if SD card is not mounted.
 */
esp_err_t sd_cache_init(void);

/**
 * @brief Check if the SD card is currently mounted.
 */
bool sd_cache_is_mounted(void);

/**
 * @brief Save a screenshot to the SD card cache.
 *
 * Writes the binary data to /sdcard/screenshot_cache/<filename>
 * and appends an entry to the upload queue.
 *
 * @param file_name   Base filename (e.g. "2025-01-15_14-30-00.bmp").
 * @param data        Pointer to binary data.
 * @param data_size   Size of data in bytes.
 * @return ESP_OK on success.
 */
esp_err_t sd_cache_save_screenshot(const char *file_name,
                                   const uint8_t *data,
                                   uint32_t data_size);

/**
 * @brief Get the number of pending uploads in the queue.
 *
 * @return Number of pending files, or 0 if empty/error.
 */
uint32_t sd_cache_pending_count(void);

/**
 * @brief Get the next pending upload from the queue (FIFO).
 *
 * Caller must free() the returned file_name.
 *
 * @param[out] out_file_name   Allocated filename string.
 * @param[out] out_file_size   File size in bytes.
 * @return ESP_OK if a pending item was found, ESP_FAIL if queue is empty.
 */
esp_err_t sd_cache_get_next_pending(char **out_file_name,
                                    uint32_t *out_file_size);

/**
 * @brief Mark a pending upload as completed (removes from queue).
 *
 * Also deletes the cached file from the SD card.
 *
 * @param file_name  The filename to mark as uploaded.
 * @return ESP_OK on success.
 */
esp_err_t sd_cache_mark_uploaded(const char *file_name);

/**
 * @brief Read a cached file's data into PSRAM.
 *
 * @param file_name   Filename within the cache directory.
 * @param[out] out_data    Allocated buffer (PSRAM), caller must free().
 * @param[out] out_size    Size of data.
 * @return ESP_OK on success.
 */
esp_err_t sd_cache_read_file(const char *file_name,
                             uint8_t **out_data,
                             uint32_t *out_size);

#ifdef __cplusplus
}
#endif
