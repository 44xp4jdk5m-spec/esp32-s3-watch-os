/**
 * @file app_lang.c
 * @brief Multi-language support — JSON-based translation lookup.
 *
 * Loads a single JSON translation file from SPIFFS at init time.
 * All lookups are O(1) after init (key→string mapping done at load time).
 *
 * SPIFFS paths (Linux FHS layout):
 *   /spiffs/usr/share/lang/en.json
 *   /spiffs/usr/share/lang/jp.json
 *   /spiffs/usr/share/lang/tc.json
 */

#include "app_lang.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "LANG";

/* ── Internal state ─────────────────────────────────────────────────── */

static cJSON *s_root = NULL;       /**< Parsed JSON root for current language */
static char    s_current[4] = {0}; /**< Current language code */

/* ── Helpers ────────────────────────────────────────────────────────── */

/**
 * Build the SPIFFS path for a language file.
 * e.g. lang_code="jp" → "/spiffs/usr/share/lang/jp.json"
 */
static void _make_path(const char *lang_code, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "/spiffs/usr/share/lang/%s.json", lang_code);
}

/**
 * Recursively walk a dot-separated key like "settings.wifi.title"
 * and return the leaf cJSON node.
 */
static cJSON *_resolve_key(cJSON *root, const char *key)
{
    if (!root || !key) return NULL;

    /* Make a mutable copy for strtok */
    char buf[256];
    strncpy(buf, key, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    cJSON *node = root;
    char *saveptr;
    char *token = strtok_r(buf, ".", &saveptr);

    while (token && node) {
        if (cJSON_IsObject(node)) {
            node = cJSON_GetObjectItem(node, token);
        } else {
            return NULL;
        }
        token = strtok_r(NULL, ".", &saveptr);
    }

    return node;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int app_lang_init(void)
{
    const char *code = g_config.lang_code;
    if (!code || code[0] == '\0') {
        code = LANG_CODE_EN;  /* default */
    }

    ESP_LOGI(TAG, "Initializing language: %s", code);
    return app_lang_set(code);
}

void app_lang_deinit(void)
{
    if (s_root) {
        cJSON_Delete(s_root);
        s_root = NULL;
    }
    s_current[0] = '\0';
}

int app_lang_set(const char *lang_code)
{
    if (!lang_code) return ESP_ERR_INVALID_ARG;

    /* Validate code */
    if (strcmp(lang_code, LANG_CODE_EN) != 0 &&
        strcmp(lang_code, LANG_CODE_JP) != 0 &&
        strcmp(lang_code, LANG_CODE_TC) != 0) {
        ESP_LOGE(TAG, "Invalid language code: %s", lang_code);
        return ESP_ERR_INVALID_ARG;
    }

    /* Build SPIFFS path */
    char path[64];
    _make_path(lang_code, path, sizeof(path));

    /* Check file exists */
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "Language file not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Read entire file */
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open language file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *raw = malloc(fsize + 1);
    if (!raw) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read_len = fread(raw, 1, fsize, f);
    fclose(f);
    raw[read_len] = '\0';

    /* Parse JSON */
    cJSON *new_root = cJSON_Parse(raw);
    free(raw);

    if (!new_root) {
        ESP_LOGE(TAG, "JSON parse error in %s", path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Replace old root */
    if (s_root) {
        cJSON_Delete(s_root);
    }
    s_root = new_root;

    /* Update state */
    strncpy(s_current, lang_code, sizeof(s_current) - 1);
    s_current[sizeof(s_current) - 1] = '\0';

    /* Persist to config */
    strncpy(g_config.lang_code, lang_code, sizeof(g_config.lang_code) - 1);
    g_config.lang_code[sizeof(g_config.lang_code) - 1] = '\0';

    ESP_LOGI(TAG, "Language set to: %s (%s)", lang_code, app_lang_name(lang_code));
    return ESP_OK;
}

const char *app_lang_get_current(void)
{
    return s_current;
}

const char *app_lang_get(const char *key)
{
    if (!s_root || !key) return key;

    cJSON *node = _resolve_key(s_root, key);
    if (!node) {
        ESP_LOGW(TAG, "Key not found: %s", key);
        return key;  /* return raw key as fallback */
    }

    if (cJSON_IsString(node)) {
        return node->valuestring;
    }

    return key;
}

const char *app_lang_name(const char *lang_code)
{
    if (!lang_code) return "Unknown";

    if (strcmp(lang_code, LANG_CODE_EN) == 0) return "English";
    if (strcmp(lang_code, LANG_CODE_JP) == 0) return "日本語";
    if (strcmp(lang_code, LANG_CODE_TC) == 0) return "繁體中文";
    return "Unknown";
}
