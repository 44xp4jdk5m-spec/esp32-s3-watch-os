/**
 * @file google_drive_uploader.h
 * @brief Upload images to Google Drive via REST API v3.
 *
 * This module uploads a binary blob (e.g. a BMP/JPEG screenshot) to
 * Google Drive using the simple upload (multipart) API.
 *
 * Authentication: Bearer token (OAuth 2.0 access token).
 * The user must obtain a token via OAuth on a PC and store it in config.
 *
 * All HTTP buffers are allocated in PSRAM to preserve DRAM.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Max length of a Google Drive folder ID string (including null). */
#define GDRIVE_FOLDER_ID_MAX    128

/** Max length of an OAuth access token string. */
#define GDRIVE_TOKEN_MAX        512

/** Max length of the upload endpoint URL override. */
#define GDRIVE_ENDPOINT_MAX     256

/** Result of a Google Drive upload operation. */
typedef enum {
    GDRIVE_OK                      = 0,  /**< Upload succeeded. */
    GDRIVE_ERR_CONFIG              = -1, /**< Missing/invalid token or folder. */
    GDRIVE_ERR_NO_WIFI            = -2, /**< WiFi not connected. */
    GDRIVE_ERR_HTTP               = -3, /**< HTTP connection/request failed. */
    GDRIVE_ERR_AUTH               = -4, /**< HTTP 401 — token expired/invalid. */
    GDRIVE_ERR_SERVER             = -5, /**< HTTP 5xx — server error. */
    GDRIVE_ERR_RESPONSE           = -6, /**< Unexpected response format. */
    GDRIVE_ERR_OOM                = -7, /**< Out of memory (PSRAM). */
    GDRIVE_ERR_TIMEOUT            = -8, /**< Connection/request timed out. */
} gdrive_result_t;

/**
 * @brief Upload a binary blob to Google Drive.
 *
 * Performs a multipart/form-data upload to Google Drive API v3.
 * Requires that WiFi is connected (caller should check app_wifi_is_connected()
 * or handle GDRIVE_ERR_NO_WIFI).
 *
 * @param file_name     The display name for the file on Google Drive
 *                      (e.g. "2025-01-15_14-30-00.bmp").
 * @param data          Pointer to the file binary data (PSRAM or DRAM).
 * @param data_size     Size of the binary data in bytes.
 * @param mime_type     MIME type string (e.g. "image/bmp", "image/jpeg").
 * @param folder_id     Google Drive folder ID to upload into, or "" for root.
 * @param access_token  OAuth 2.0 Bearer token.
 * @return GDRIVE_OK on success, or an error code.
 */
gdrive_result_t google_drive_upload_file(const char *file_name,
                                         const uint8_t *data,
                                         uint32_t data_size,
                                         const char *mime_type,
                                         const char *folder_id,
                                         const char *access_token);

/**
 * @brief Convert a gdrive_result_t to a human-readable string.
 */
const char *google_drive_result_str(gdrive_result_t res);

#ifdef __cplusplus
}
#endif
