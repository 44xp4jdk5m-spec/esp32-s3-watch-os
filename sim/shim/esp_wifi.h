/**
 * @file esp_wifi.h
 * @brief Host shim for the ESP-IDF Wi-Fi driver header.
 *
 * The simulator has no radio; the Settings network page only includes this
 * header (all access goes through app_wifi_*).  Keep the Wi-Fi auth-mode
 * constants so any UI that references them still compiles.
 */
#pragma once

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA2_ENTERPRISE,
    WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK,
} wifi_auth_mode_t;

typedef struct {
    unsigned char bssid[6];
    char          ssid[33];
    unsigned char primary;
    int8_t        rssi;
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;
