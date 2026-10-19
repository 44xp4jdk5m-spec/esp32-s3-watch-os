/**
 * @file app_config.h
 * @brief Persistent JSON config stored in SPIFFS.
 *
 * Saves/loads a JSON config file to/from the "storage" SPIFFS partition.
 * Config survives reboots.  Uses ESP-IDF's built-in cJSON.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ── Config field size limits ───────────────────────────────────────── */
#define APP_CFG_GDRIVE_TOKEN_MAX     512   /**< OAuth token max length. */
#define APP_CFG_GDRIVE_FOLDER_ID_MAX 128   /**< Folder ID max length. */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Config structure ──────────────────────────────────────────────── */

typedef struct {
    /** Enable/disable WiFi. When false, skip RTC calibration on boot. */
    bool    wifi_enabled;

    /** Enable/disable BLE (ANCS for iPhone notifications). */
    bool    ble_enabled;

    /** ANCS (notification) switch — independent of BLE master switch. */
    bool    ancs_enabled;

    /** Airplane mode: turn off all radios (WiFi + BLE/ANCS). */
    bool    airplane_mode;

    /** Display brightness 10–100 (percent). */
    uint8_t brightness;

    /** Font size scale: 0 = Small, 1 = Normal (default), 2 = Big. */
    uint8_t font_scale;

    /** Always-On Display: when true, never dim/off the display. */
    bool    always_on_display;

    /** Skip the boot-up splash screen: when true, boot straight to the
     *  watch face once WiFi/time init finishes (no boot ring shown). */
    bool    skip_boot_screen;

    /** Wake-on-raise: when true, the watch auto-wakes when lifted while the
     *  display is off (ESP-DL IMU classifier, active only with display off). */
    bool    wake_on_raise_enabled;

    /** Light sleep: when true (and wake-on-raise is off), the ESP32-S3 halts
     *  in light sleep while the display is off, waking on touch / GPIO0 /
     *  GPIO10 / periodic housekeeping.  Fallback flag: if light sleep proves
     *  unstable on this unit, set false to restore the old "CPU keeps polling
     *  with the display off" behaviour. */
    bool    light_sleep_enabled;

    /** Wrist-flick back: when true, a wrist flick acts as the "back" button
     *  while using a sub-screen (ESP-DL IMU classifier, display on only). */
    bool    wrist_flick_enabled;

    /** Wrist-flick back: detection confidence threshold (%). 20-99, default 96
     *  (trained model flicks saturate at ~0.99, not-flick <0.06, so 96% is a
     *  strict but effective default).  Higher = stricter, so fewer false
     *  triggers but may miss weaker flicks.  Adjustable from Settings ->
     *  Accessibility & Gesture. */
    uint8_t wrist_flick_confidence_pct;

    /** Motion pointer (air mouse): when true, an on-screen cursor follows the
     *  watch tilt (QMI8658), double-tap on the watch body clicks, and resting
     *  near a screen edge auto-scrolls.  System-level, in components/os/. */
    bool    air_pointer_enabled;

    /** Menu style: 0 = Honeycomb (grid), 1 = List view. */
    uint8_t menu_style;

    /** Dim timeout in seconds before display enters half-brightness.
     *  One of 5, 15, 30, 60, 300. Default 20. */
    uint16_t dim_timeout_sec;

    /** Timezone UTC offset in hours (e.g. 8 = HKT, 0 = GMT, -5 = EST). */
    int8_t   timezone_offset;

    /** Region index into app_time_region table (0 = Hong Kong). */
    uint8_t  region_index;

    /** NTP URL index within the selected region's URL list. */
    uint8_t  ntp_url_index;

    /** CPU protection: when true, monitor CPU/PMIC temps and shutdown
     *  WiFi, BLE and charging if thresholds are exceeded. */
    bool     cpu_protection_enabled;

    /** AXP2101 PMIC temperature threshold in °C (30–80, step 5). */
    uint8_t  power_chips_temp_threshold;

    /** ESP32-S3 CPU temperature threshold in °C (30–80, step 5). */
    uint8_t  cpu_temp_threshold;

    /** Honeycomb menu: only show distinct icons (no repeats). */
    bool     honeycomb_distinct_icons;

    /** Honeycomb menu: honeycomb layout mode. 0 = hex spiral, 1 = vertical grid. */
    uint8_t  honeycomb_layout_mode;

    /** Honeycomb menu: animate vertical grid icons row-by-row on entry. */
    bool     honeycomb_vertical_entry_animation;

    /** Honeycomb menu: animate hex-spiral icons (scale + big-circle close). */
    bool     honeycomb_hex_entry_animation;

    /** Honeycomb menu: tighten icon gap (+25%). When true, grid spacing
     *  compresses slightly with icon size so gaps shrink; when false,
     *  original spacing is used. Applies to hex and vertical layouts. */
    bool     honeycomb_gap_adjust;

    /** Honeycomb menu: icon order. honeycomb_icon_order[slot] = icon id
     *  (0..47) shown at that grid slot in distinct-icons mode.  A permutation
     *  of 0..47; edited by the icon rearrange mode and persisted as JSON. */
    uint8_t  honeycomb_icon_order[48];

    /** Charge limit at 80%: when true, stop charging at 80%. */
    bool     charge_limit_80;

    /** Optimized charging: when true (and the 80% limit is off), adapt the
     *  AXP2101 charge current so the battery finishes ~1 h before the user's
     *  typical morning use, capped at 0.7 C. Never below the 25 mA minimum. */
    bool     optimized_charging_enabled;

    /** Low power mode: disable animations, 15 FPS, 5s dim, 80% brightness. */
    bool     low_power_mode;

    /* ── Sound & Haptics ────────────────────────────────────────── */
    /** Silent mode: when true, suppress all sound and vibration. */
    bool     silent_mode;

    /* ── Haptics / Vibration ────────────────────────────────────── */
    /** Vibration strength 0-255 (PWM duty cycle). */
    uint8_t  vibrator_strength;

    /** Vibration pattern index (hw_vibe_pattern_t). */
    uint8_t  vibrator_pattern;

    /** Vibration duration in milliseconds (50-5000). */
    uint16_t vibrator_duration_ms;

    /** Unix timestamp of last time battery reached 100%. */
    int64_t  last_charged_to_100_ts;

    /** Log enabled (Debug page). */
    bool     log_enabled;

    /** LVGL CPU monitor enabled (Debug page). */
    bool     cpu_monitor_enabled;

    /* ── Storage / Google Drive ─────────────────────────────────── */
    /** Enable Google Drive auto-upload of screenshots. */
    bool     storage_gdrive_enabled;

    /** Google Drive OAuth 2.0 access token (Bearer token). */
    char     storage_gdrive_token[APP_CFG_GDRIVE_TOKEN_MAX];

    /** Google Drive folder ID to upload into (empty = root). */
    char     storage_gdrive_folder_id[APP_CFG_GDRIVE_FOLDER_ID_MAX];

    /** Upload as BMP (true) or JPEG (false). BMP is lossless. */
    bool     storage_gdrive_upload_bmp;

    /* ── USB Mass Storage (TinyUSB MSC) ─────────────────────────── */
    /** Enable USB Mass Storage Class (SD card via USB OTG GPIO19/20). */
    bool     usb_msc_enabled;

    /* ── WiFi File Server ────────────────────────────────────────── */
    /** Enable WiFi file server (browse SD card via HTTP). */
    bool     wifi_fs_enabled;

    /* ── WiFi credentials (selected from the Settings > WiFi list) ─ */
    /** SSID of the network the watch connects to. Empty = factory default. */
    char     wifi_ssid[33];

    /** Password of wifi_ssid. Empty = open network. */
    char     wifi_password[65];

    /* ── OTA over WiFi ───────────────────────────────────────────── */
    /** Enable OTA: when true, the Settings > System > OTA page can download
     *  and install a new firmware image over WiFi. */
    bool     ota_enabled;

    /** HTTP URL of the firmware .bin to download (e.g. http://192.168.1.5:8080/lvgl_demo_v9.bin). */
    char     ota_url[128];

    /* ── Language ────────────────────────────────────────────────── */
    /** Language code: "en" (English), "jp" (Japanese), "tc" (Trad. Chinese). */
    char     lang_code[4];

    /* ── Watch face ──────────────────────────────────────────────── */
    /** Active watch-face id (index into the watch-face registry). */
    uint8_t  watch_face_id;

    /** Flux watch face — top panel color (0xRRGGBB). Set by the editor. */
    uint32_t flux_color_top;

    /** Flux watch face — bottom panel color (0xRRGGBB). Set by the editor. */
    uint32_t flux_color_bot;

    /** Analog Hands watch face — colour slots (0xRRGGBB), set by the
     *  watch-face editor.  The centre pin is recoloured to match the
     *  background colour (slot 0). */
    uint32_t hands_face_bg;     /**< background (+ centre pin) colour */
    uint32_t hands_face_hour;   /**< hour hand (+ inlay) colour */
    uint32_t hands_face_min;    /**< minute hand (+ inlay) colour */
    uint32_t hands_face_sec;    /**< second hand colour */
} app_config_t;

/* ── Global config instance ────────────────────────────────────────── */

/** The currently loaded config.  Read on boot, saved before shutdown. */
extern app_config_t g_config;

/* ── API ────────────────────────────────────────────────────────────── */

/**
 * @brief Mount SPIFFS and load config from flash.
 *
 * Must be called once during boot, BEFORE any module reads g_config.
 *
 * @return ESP_OK or error.
 */
esp_err_t app_config_init(void);

/**
 * @brief Unmount SPIFFS and flush any pending writes.
 */
void app_config_deinit(void);

/**
 * @brief Save current g_config to the JSON file on SPIFFS.
 *
 * @return ESP_OK or error.
 */
esp_err_t app_config_save(void);

/**
 * @brief Reload config from flash (useful after external modification).
 *
 * @return ESP_OK or error.
 */
esp_err_t app_config_load(void);

/* ── SD-card config backup / restore ───────────────────────────────── */

/** SD-card backup directory for the system config (Linux FHS layout). */
#define APP_CFG_SD_DIR  "/sdcard/watchos/sys/config"
#define APP_CFG_SD_FILE APP_CFG_SD_DIR "/watch_config.json"

/**
 * @brief Copy the current config file from SPIFFS to the SD card.
 *
 * Creates /sdcard/watchos/sys/config/ if needed and overwrites
 * watch_config.json with the current SPIFFS config.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND (SD card not mounted or no config
 *         file yet), or ESP_FAIL (I/O error).
 */
esp_err_t app_config_backup_to_sd(void);

/**
 * @brief Copy the config file from the SD card back to SPIFFS and reload
 *        it into g_config (so the new settings apply live).
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND (no backup on the SD card), or
 *         ESP_FAIL (I/O error).
 */
esp_err_t app_config_restore_from_sd(void);

#ifdef __cplusplus
}
#endif
