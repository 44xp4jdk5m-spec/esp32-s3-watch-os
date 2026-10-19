/**
 * @file sim_services.c
 * @brief Host stubs for the OS services the Settings app queries.
 *
 * The real implementations talk to Wi-Fi, BLE, the SD card, USB-MSC, OTA
 * and NVS.  None of that exists in the simulator, so every call returns a
 * plausible "idle / empty" value and the UI renders its normal look.
 */
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_dictionary.h"
#include "app_power.h"
#include "app_time_region.h"
#include "app_usage.h"
#include "app_wifi.h"
#include "battery_history.h"
#include "esp_err.h"
#include "gesture_recorder.h"
#include "hw_display.h"
#include "hw_vibrator.h"
#include "js_app_registry.h"
#include "list_view_menu.h"
#include "ota_manager.h"
#include "power_stats.h"
#include "sd_cache_manager.h"
#include "storage_manager.h"
#include "tinyusb_msc.h"
#include "wifi_file_server.h"
#include "wrist_flick.h"
#include "air_pointer.h"

/* ── Wi-Fi: no radio, empty scan results ──────────────────────────── */
bool app_wifi_scan_begin(void)                 { return false; }
bool app_wifi_scan_is_done(void)               { return true;  }
int  app_wifi_scan_get_count(void)             { return 0;     }

bool app_wifi_scan_get(int idx, app_wifi_ap_t *out)
{
    (void)idx;
    if (out) memset(out, 0, sizeof(*out));
    return false;
}

void app_wifi_connect_to(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
}

bool        app_wifi_is_connecting(void)       { return false; }
bool        app_wifi_connect_is_done(void)     { return true;  }
bool        app_wifi_connect_ok(void)          { return false; }
const char *app_wifi_get_connected_ssid(void)  { return "";    }
void        app_wifi_idle_deinit(void)         { }

/* ── Time regions ─────────────────────────────────────────────────── */
static const time_region_t s_regions[] = {
    { "Hong Kong", "UTC+8 HKT", 8, 1, { "pool.ntp.org" } },
    { "UTC",       "UTC",       0, 1, { "pool.ntp.org" } },
};

uint8_t app_time_region_get_count(void)
{
    return (uint8_t)(sizeof(s_regions) / sizeof(s_regions[0]));
}

const time_region_t *app_time_region_get(uint8_t idx)
{
    if (idx >= app_time_region_get_count()) return NULL;
    return &s_regions[idx];
}

const char *app_time_region_get_ntp_url(uint8_t region_idx, uint8_t ntp_idx)
{
    const time_region_t *r = app_time_region_get(region_idx);
    if (!r || ntp_idx >= r->ntp_count) return "";
    return r->ntp_urls[ntp_idx];
}

int8_t app_time_region_get_tz(uint8_t idx)
{
    const time_region_t *r = app_time_region_get(idx);
    return r ? r->utc_offset_hours : 0;
}

/* ── Usage statistics: no history on the host ─────────────────────── */
uint32_t app_usage_cat_sec(int day_ago, app_usage_cat_t cat)
{
    (void)day_ago;
    (void)cat;
    return 0;
}

uint32_t app_usage_total_sec(int day_ago)      { (void)day_ago; return 0; }
uint32_t app_usage_week_total_sec(void)        { return 0; }
uint32_t app_usage_app_sec(int reg_index)      { (void)reg_index; return 0; }

/* ── Menu style ───────────────────────────────────────────────────── */
static menu_style_t s_menu_style = (menu_style_t)0;

void app_menu_set_style(menu_style_t style) { s_menu_style = style; }
menu_style_t app_menu_get_style(void)       { return s_menu_style; }

/* ── Brightness ───────────────────────────────────────────────────── */
void app_power_set_brightness_smooth(uint8_t target, int step_ms)
{
    (void)step_ms;
    hw_display_set_brightness(target);
}

/* ── Battery history ──────────────────────────────────────────────── */
int battery_history_load(battery_history_entry_t *entries)
{
    (void)entries;
    return 0;   /* no samples */
}

/* ── Gesture recorder: idle ───────────────────────────────────────── */
static gesture_rec_mode_t s_gesture_mode = (gesture_rec_mode_t)0;

void gesture_recorder_set_mode(gesture_rec_mode_t mode) { s_gesture_mode = mode; }
gesture_rec_mode_t gesture_recorder_get_mode(void)      { return s_gesture_mode; }
bool gesture_recorder_start(void)                       { return false; }
void gesture_recorder_stop(void)                        { }
bool gesture_recorder_is_recording(void)                { return false; }
bool gesture_recorder_mark_flick(void)                  { return false; }
uint32_t gesture_recorder_flick_count(void)             { return 0; }
uint32_t gesture_recorder_sample_count(void)            { return 0; }
float    gesture_recorder_peak_gyro(void)               { return 0.0f; }
void     gesture_recorder_clear(void)                   { }
esp_err_t gesture_recorder_save(const char *filename)   { (void)filename; return ESP_OK; }

/* ── Haptics ──────────────────────────────────────────────────────── */
const char *hw_vibe_pattern_names[HW_VIBE_PATTERN_COUNT] = {
    "Click", "Double click", "Triple click", "Heartbeat", "Rising",
    "Decaying", "Pulsing", "Siren", "SOS", "Buzz",
};

esp_err_t hw_vibrator_play_pattern(hw_vibe_pattern_t pattern,
                                   uint8_t strength, uint16_t duration_ms)
{
    (void)pattern;
    (void)strength;
    (void)duration_ms;
    return ESP_OK;
}

/* ── JS app storage ───────────────────────────────────────────────── */
int  js_app_registry_delete(int index)       { (void)index; return -1; }
int  js_app_registry_clear_data(int index)   { (void)index; return -1; }
bool js_app_registry_has_data(int index)     { (void)index; return false; }

/* ── OTA ──────────────────────────────────────────────────────────── */
ota_state_t ota_manager_get_state(void)   { return OTA_IDLE; }
int         ota_manager_get_progress(void) { return 0; }
bool        ota_manager_start_download(void) { return false; }
void        ota_manager_install(void)     { }

/* ── Power statistics ─────────────────────────────────────────────── */
bool power_stats_get(int current_pct, float *avg_ma, float *hours, int *peak_pct)
{
    (void)current_pct;
    if (avg_ma)   *avg_ma = 0.0f;
    if (hours)    *hours = 0.0f;
    if (peak_pct) *peak_pct = 0;
    return false;
}

/* ── Storage / USB-MSC / file server ──────────────────────────────── */
bool     sd_cache_is_mounted(void)          { return false; }
uint32_t storage_manager_pending_count(void) { return 0; }

esp_err_t tinyusb_msc_init(void)            { return ESP_ERR_NOT_SUPPORTED; }
void      tinyusb_msc_deinit(void)          { }

esp_err_t   wifi_file_server_start(void)    { return ESP_ERR_NOT_SUPPORTED; }
void        wifi_file_server_stop(void)     { }

/* ── Wrist-flick / air-pointer gestures ───────────────────────────── */
esp_err_t wrist_flick_start(void)           { return ESP_OK; }

esp_err_t air_pointer_start(void)           { return ESP_OK; }
void      air_pointer_stop(void)            { }

/* ── Config backup to SD ──────────────────────────────────────────── */
esp_err_t app_config_backup_to_sd(void)     { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t app_config_restore_from_sd(void)  { return ESP_ERR_NOT_SUPPORTED; }

/* ── Keyboard dictionary ──────────────────────────────────────────── */
esp_err_t app_dict_init(void)               { return ESP_OK; }
bool      app_dict_ready(void)              { return false; }
int       app_dict_suggest(const char *prefix, app_dict_entry_t *out, int max_n)
{
    (void)prefix;
    (void)out;
    (void)max_n;
    return 0;
}

int app_dict_correct(const char *word, app_dict_entry_t *out, int max_n)
{
    (void)word;
    (void)out;
    (void)max_n;
    return 0;
}
