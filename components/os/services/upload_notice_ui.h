/**
 * @file upload_notice_ui.h
 * @brief Temporary on-screen notice for upload success/failure.
 *
 * Shows a small banner at the bottom of the screen for ~2 seconds,
 * then auto-dismisses.  The user can later replace this with a more
 * polished UI component.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Result type for upload notices. */
typedef enum {
    UPLOAD_NOTICE_SUCCESS,   /**< Upload completed successfully. */
    UPLOAD_NOTICE_FAILED,    /**< Upload failed. */
    UPLOAD_NOTICE_CACHED,    /**< Saved to SD card (offline). */
} upload_notice_type_t;

/**
 * @brief Show an upload result notice on screen.
 *
 * Creates a temporary LVGL label at the bottom of the active screen.
 * It auto-dismisses after 3 seconds.
 *
 * Must be called under the LVGL display lock.
 *
 * @param type    Type of notice to display.
 * @param message Optional custom message string (can be NULL for default).
 */
void upload_notice_show(upload_notice_type_t type, const char *message);

/**
 * @brief Hide any active upload notice immediately.
 */
void upload_notice_hide(void);

#ifdef __cplusplus
}
#endif
