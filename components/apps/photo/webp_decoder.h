/**
 * @file webp_decoder.h
 * @brief Custom WEBP image decoder for LVGL (VP8 / VP8L)
 *
 * Registers an LVGL image decoder that handles .webp files using a
 * lightweight embedded WEBP decoder.  Decoded output is allocated from
 * PSRAM (MALLOC_CAP_SPIRAM) to conserve internal DRAM.
 *
 * Supported formats:
 *   - WEBP lossy (VP8 key frame)
 *   - WEBP lossless (VP8L)
 *
 * Call webp_decoder_init() once at startup to register the decoder.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the custom WEBP decoder with LVGL.
 *
 * Safe to call multiple times — will only register once.
 */
void webp_decoder_init(void);

/**
 * @brief Parse a WEBP container header.
 * @retval 0 on success, -1 on error.
 */
int webp_parse_header(const uint8_t *data, size_t len,
                      int *width, int *height, int *is_lossless);

/**
 * @brief Decode a VP8 key frame to RGB888.
 * @param vp8_data  Pointer to VP8 bitstream (starting at frame tag).
 * @param vp8_size  Size of VP8 data.
 * @param rgb_out   Output buffer (width * height * 3 bytes).
 * @param width,height  Image dimensions.
 * @retval 0 on success, -1 on error.
 */
int vp8_decode_key_frame(const uint8_t *vp8_data, size_t vp8_size,
                          uint8_t *rgb_out, int width, int height);

/**
 * @brief Decode a VP8L frame to RGB888 (basic implementation).
 */
int vp8l_decode_frame(const uint8_t *data, size_t data_size,
                       uint8_t *rgb_out, int width, int height);

#ifdef __cplusplus
}
#endif
