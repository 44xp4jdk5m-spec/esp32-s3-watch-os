/**
 * @file screen_capture.h
 * @brief LVGL screen capture to PSRAM buffer.
 *
 * Captures the current LVGL display framebuffer into a PSRAM-backed
 * RGB565 bitmap.  The caller must manage the returned buffer lifetime.
 *
 * All primary memory is allocated from PSRAM (MALLOC_CAP_SPIRAM)
 * to preserve internal DRAM for LVGL DMA descriptors.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Screen capture buffer, stored in PSRAM ────────────────────────── */

/** Opaque handle to a captured screenshot in PSRAM. */
typedef struct screen_capture_t screen_capture_t;

/**
 * @brief Capture the current LVGL screen into a PSRAM buffer.
 *
 * Allocates the buffer from PSRAM (MALLOC_CAP_SPIRAM).  The caller
 * MUST call screen_capture_free() when done.
 *
 * The capture is performed under the LVGL display lock to ensure a
 * consistent frame.
 *
 * @param[out] out_cap  Pointer to receive the capture handle.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if PSRAM allocation fails.
 */
esp_err_t screen_capture_take(screen_capture_t **out_cap);

/**
 * @brief Get a pointer to the raw RGB565 pixel data in the capture.
 *
 * @param cap  Capture handle from screen_capture_take().
 * @return Pointer to pixel data (owned by the capture, do NOT free).
 */
const uint8_t *screen_capture_get_data(const screen_capture_t *cap);

/**
 * @brief Get the size of the raw pixel data in bytes.
 */
uint32_t screen_capture_get_data_size(const screen_capture_t *cap);

/**
 * @brief Get the capture width in pixels.
 */
uint16_t screen_capture_get_width(const screen_capture_t *cap);

/**
 * @brief Get the capture height in pixels.
 */
uint16_t screen_capture_get_height(const screen_capture_t *cap);

/**
 * @brief Free a capture buffer previously returned by screen_capture_take().
 */
void screen_capture_free(screen_capture_t *cap);

/**
 * @brief Encode a raw RGB565 capture to a BMP buffer in PSRAM.
 *
 * BMP is lossless and requires no external library — just a simple
 * 54-byte header + pixel data.  Produces large files (~400 KB for
 * a 410×502 screen) but is the most portable format.
 *
 * @param cap             Source capture (RGB565).
 * @param[out] out_bmp    Pointer to receive the BMP buffer (PSRAM).
 * @param[out] out_size   Size of the BMP data in bytes.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails.
 */
esp_err_t screen_capture_encode_bmp(const screen_capture_t *cap,
                                    uint8_t **out_bmp,
                                    uint32_t *out_size);

/**
 * @brief Encode a raw RGB565 capture to a JPEG buffer in PSRAM.
 *
 * JPEG is lossy but much smaller (~30-50 KB for a 410×502 screen).
 * Uses ESP-IDF's hardware JPEG encoder.
 *
 * @param cap       Source capture (RGB565).
 * @param quality   JPEG quality 1–100 (85 recommended).
 * @param[out] out_jpeg     Pointer to receive the JPEG buffer (PSRAM).
 * @param[out] out_jpeg_size Size of the JPEG data in bytes.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails.
 *         ESP_ERR_NOT_SUPPORTED if JPEG encoder is not available.
 */
esp_err_t screen_capture_encode_jpeg(const screen_capture_t *cap,
                                     int quality,
                                     uint8_t **out_jpeg,
                                     uint32_t *out_jpeg_size);

#ifdef __cplusplus
}
#endif
