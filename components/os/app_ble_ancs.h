/**
 * @file app_ble_ancs.h
 * @brief Application-level BLE ANCS (Apple Notification Center Service) wrapper.
 *
 * Initializes ESP32 as a BLE peripheral that iPhone connects to.
 * Receives iOS notifications via ANCS and delivers them through callbacks.
 *
 * This module handles:
 *   - BLE controller & Bluedroid init
 *   - Advertising with HID service UUID (required by iOS for ANCS pairing)
 *   - GATT client operations to discover iPhone's ANCS service
 *   - Pairing & bonding with passkey display
 *   - Reconnection on disconnect
 *
 * Based on ESP-IDF ble_ancs example (bluedroid/ble/ble_ancs).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ble_ancs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Connection status ────────────────────────────────────────────── */

/** Is iPhone currently connected via BLE? */
extern bool g_ancs_connected;

/** Has ANCS service been discovered and notifications subscribed? */
extern bool g_ancs_ready;

/** Passkey being displayed for pairing (6 digits as string) */
extern char g_ancs_passkey[8];

/* ── Latest notification (thread-safe snapshot) ───────────────────── */

/** Latest complete notification received (title + message populated) */
extern ancs_notification_t g_latest_notification;

/** Flag: a new notification is available (app_ui reads & clears this) */
extern bool g_new_notification_pending;

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * @brief Initialize BLE and start advertising for ANCS.
 *
 * Must be called once after NVS is initialized.
 * Non-blocking — all BLE operations are event-driven.
 *
 * @param device_name  BLE device name visible on iPhone
 * @return ESP_OK on success, or error code
 */
esp_err_t app_ble_ancs_init(const char *device_name);

/**
 * @brief Process pending BLE events (call from main loop periodically).
 */
void app_ble_ancs_poll(void);

/**
 * @brief Stop BLE advertising and disconnect (for thermal protection).
 *
 * Lightweight — stops advertising only.  Call app_ble_ancs_restart()
 * to resume later.
 */
void app_ble_ancs_stop(void);

/**
 * @brief Resume BLE advertising after a stop (thermal recovery).
 */
void app_ble_ancs_restart(void);

/**
 * @brief Cut the BLE radio while the display is off / in standby.
 *
 * Always stops advertising and drops any live link, so there are NO scheduled
 * RF events to wake the baseband during light sleep (the single biggest idle
 * drain on this watch).  Call on display-off/standby entry.
 */
void app_ble_ancs_suspend(void);

/**
 * @brief Restore BLE after waking from standby.
 *
 * Re-arms advertising; reconnection runs through the normal GAP flow.
 */
void app_ble_ancs_resume(void);

#ifdef __cplusplus
}
#endif
