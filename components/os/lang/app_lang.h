/**
 * @file app_lang.h
 * @brief Multi-language support using JSON files in SPIFFS.
 *
 * Loads language strings from /spiffs/usr/share/lang/{en,jp,tc}.json.
 * Provides a simple key-based lookup: app_lang_get("settings.title")
 *
 * Language is persisted in g_config.lang_code ("en", "jp", "tc").
 */

#pragma once

#include "cJSON.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Language codes ────────────────────────────────────────────────── */

#define LANG_CODE_EN  "en"
#define LANG_CODE_JP  "jp"
#define LANG_CODE_TC  "tc"

#define LANG_COUNT     3

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the language system.
 *
 * Loads the JSON file for the currently configured language from SPIFFS.
 * Must be called after app_config_init() and SPIFFS mount.
 *
 * @return ESP_OK on success, or error code.
 */
int app_lang_init(void);

/**
 * @brief De-initialize and free language resources.
 */
void app_lang_deinit(void);

/**
 * @brief Switch to a different language.
 *
 * Loads the new JSON file and updates g_config.lang_code.
 *
 * @param lang_code  One of LANG_CODE_EN, LANG_CODE_JP, LANG_CODE_TC.
 * @return ESP_OK on success, or error code.
 */
int app_lang_set(const char *lang_code);

/**
 * @brief Get the current language code.
 * @return "en", "jp", or "tc".
 */
const char *app_lang_get_current(void);

/**
 * @brief Look up a localized string by dot-separated key.
 *
 * Example keys:
 *   "settings.title"     → "Settings" / "設定" / "設定"
 *   "wifi.switch_label"  → "Wi-Fi"
 *   "display.brightness" → "Brightness" / "明るさ" / "亮度"
 *
 * @param key  Dot-separated JSON path (e.g. "settings.wifi")
 * @return The localized string, or key itself if not found.
 */
const char *app_lang_get(const char *key);

/**
 * @brief Get the display name of a language in its own language.
 *
 * @param lang_code  "en", "jp", or "tc"
 * @return "English", "日本語", or "繁體中文"
 */
const char *app_lang_name(const char *lang_code);

#ifdef __cplusplus
}
#endif
