/**
 * @file wifi_file_server.h
 * @brief WiFi-based file server — browse SD card via HTTP in browser.
 *
 * When activated, the watch starts an HTTP server on port 8080 that
 * serves files from the SD card.  Connect to the watch's IP address
 * from any browser to browse and download files.
 *
 * Only active when WiFi is connected and the feature is enabled in
 * Settings → Storage → SD Card → "WiFi File Server".
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the WiFi file server.
 *
 * Creates an HTTP server on port 8080 serving files from /sdcard/.
 * Requires WiFi to be connected.  Safe to call multiple times
 * (idempotent — returns ESP_OK if already running).
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if WiFi not connected.
 */
esp_err_t wifi_file_server_start(void);

/**
 * @brief Stop the WiFi file server.
 */
void wifi_file_server_stop(void);

/**
 * @brief Check if the file server is currently running.
 */
bool wifi_file_server_is_running(void);

/**
 * @brief Get the server's IP address as a string.
 *
 * @param buf  Buffer to write the IP string (must be at least 16 bytes).
 * @return Pointer to buf, or NULL if not running.
 */
const char *wifi_file_server_get_ip(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
