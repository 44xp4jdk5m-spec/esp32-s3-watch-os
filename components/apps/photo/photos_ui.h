/**
 * @file photos_ui.h
 * @brief Photos app UI — multi-format streaming viewer (PSRAM)
 *
 * Decodes BMP, JPEG/JPG, PNG, GIF (via LVGL built-in decoders) and
 * WEBP (via custom embedded decoder).  All decoded buffers use PSRAM.
 *
 * Gallery uses a streaming-window approach: only visible thumbnails
 * are decoded; off-screen images are freed to conserve memory.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the photos UI.
 *
 * Registers the custom WEBP decoder (once), scans SD card for image
 * files (.bmp, .jpg, .jpeg, .png, .gif, .webp), builds the home
 * screen panel selector, and loads it.
 */
void photos_ui_create(void);

/**
 * @brief Destroy the photos UI and free all resources.
 */
void photos_ui_destroy(void);

/**
 * @brief Check if the photos UI is currently active.
 */
bool photos_ui_is_active(void);

/**
 * @brief Handle back navigation.
 *
 * From gallery → home screen (animated).  From home screen → exit.
 */
void photos_ui_handle_back(void);

#ifdef __cplusplus
}
#endif
