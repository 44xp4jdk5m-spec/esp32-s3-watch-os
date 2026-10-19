/**
 * @file app_wifi.h
 * @brief Direct WiFi connect → NTP time sync → RTC calibration → full cleanup.
 *
 * Respects g_config.wifi_enabled — when false, skips entirely.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of access points kept from one scan. */
#define APP_WIFI_SCAN_MAX_APS   16

/** One discovered access point. */
typedef struct {
    char    ssid[33];    /**< Network name (NUL-terminated). */
    int8_t  rssi;        /**< Signal strength in dBm (-100 … 0). */
    uint8_t authmode;    /**< esp_wifi auth mode; WIFI_AUTH_OPEN (0) = no password. */
} app_wifi_ap_t;

/**
 * @brief Connect WiFi, sync NTP, set RTC.  Respects g_config.wifi_enabled.
 *        Call once during boot.
 */
void app_wifi_start_provisioning(void);

/**
 * @brief Check if WiFi is currently connected.
 */
bool app_wifi_is_connected(void);

/**
 * @brief Turn WiFi on/off at runtime (respects config).
 *        When turning off, disconnects and deinits WiFi.
 * @param enable  true = connect, false = disconnect & deinit
 */
void app_wifi_set_enabled(bool enable);

/**
 * @brief Deinit WiFi if it is NOT currently connected.
 *        Call when leaving a screen that only needed WiFi for scanning,
 *        to reclaim internal RAM/DMA.  An active connection is kept.
 */
void app_wifi_idle_deinit(void);

/* ── Runtime scan (Settings > WiFi list) ───────────────────────────── */

/**
 * @brief Start an asynchronous WiFi scan in a background task.
 *        Results are available via app_wifi_scan_is_done/get_count/get.
 * @return true if the scan was started, false if one is already running.
 */
bool app_wifi_scan_begin(void);

/** @return true when the last scan has finished (success or failure). */
bool app_wifi_scan_is_done(void);

/** @return Number of access points found by the last scan. */
int  app_wifi_scan_get_count(void);

/**
 * @brief Copy one scan result.
 * @param idx  0-based index into the scan results.
 * @param out  Receives the AP entry.
 * @return true on success.
 */
bool app_wifi_scan_get(int idx, app_wifi_ap_t *out);

/* ── Runtime connect (Settings > WiFi list) ────────────────────────── */

/**
 * @brief Save the SSID/password to persistent config and start an
 *        asynchronous connection attempt in a background task.
 *        On success the WiFi stays connected; on failure it is torn down.
 */
void app_wifi_connect_to(const char *ssid, const char *password);

/** @return true while a connect attempt is in progress. */
bool app_wifi_is_connecting(void);

/** @return true when the last connect attempt completed. */
bool app_wifi_connect_is_done(void);

/** @return true if the last connect attempt succeeded. */
bool app_wifi_connect_ok(void);

/** @return SSID of the network currently connected (may be empty). */
const char *app_wifi_get_connected_ssid(void);

#ifdef __cplusplus
}
#endif
