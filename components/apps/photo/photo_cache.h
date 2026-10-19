/**
 * @file photo_cache.h
 * @brief SD card thumbnail cache for the Photos app.
 *
 * Decodes full-size images once, resizes to thumbnail, and stores raw
 * RGB565 data on the SD card.  Subsequent loads read the cached raw
 * data directly — no decoder chain needed, instant display.
 *
 * Cache directory: /sdcard/shots/.cache/
 * Cache files:     <original_name>.cch
 * Cache format:    4-byte magic + 2-byte w + 2-byte h + raw RGB565 data
 *
 * BMP fix: if a BMP has negative biHeight (top-down), it is corrected
 * in-place before decoding so LVGL's built-in BMP decoder works.
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the cache file path for a source image.
 * @param src_path  Original file path (e.g. "S:/shots/lake.jpg")
 * @param out_path  Output buffer for cache path
 * @param out_sz    Output buffer size
 */
void photo_cache_get_path(const char *src_path, char *out_path, size_t out_sz);

/**
 * @brief Check if a valid cached thumbnail exists.
 */
bool photo_cache_exists(const char *src_path);

/**
 * @brief Decode a full-size image, resize to thumbnail, save to SD card.
 *
 * Handles WEBP (custom decoder), BMP (with biHeight fix for old files),
 * JPEG, PNG, and GIF (via LVGL built-in decoders).
 *
 * On success, also returns the resized draw_buf (PSRAM allocated).
 * Caller must free with lv_draw_buf_destroy().
 *
 * @param src_path  Original file path
 * @param thumb_w   Desired thumbnail width
 * @param thumb_h   Desired thumbnail height
 * @return lv_draw_buf_t*  Resized thumbnail, or NULL on failure
 */
lv_draw_buf_t *photo_cache_generate(const char *src_path,
                                     int thumb_w, int thumb_h);

/**
 * @brief Load a cached thumbnail from SD card into PSRAM.
 *
 * Allocates the pixel data and draw_buf from PSRAM.
 * Caller must free with lv_draw_buf_destroy().
 *
 * @param src_path  Original file path (to locate the cache)
 * @return lv_draw_buf_t*  Loaded draw_buf, or NULL on failure
 */
lv_draw_buf_t *photo_cache_load(const char *src_path);

#ifdef __cplusplus
}
#endif
