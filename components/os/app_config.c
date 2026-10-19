/**
 * @file app_config.c
 * @brief Persistent JSON config stored in SPIFFS partition.
 *
 * Mounts the "storage" SPIFFS partition and reads/writes a JSON config
 * file at /spiffs/etc/watch_config.json (Linux FHS layout: config lives
 * under /etc, read-only assets under /usr/share, runtime state under /var).
 * Uses cJSON (bundled with ESP-IDF).
 */

#include "app_config.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "CONFIG";

#define CONFIG_FILENAME  "/spiffs/etc/watch_config.json"
#define SPIFFS_PARTITION "storage"
#define SPIFFS_BASE_PATH "/spiffs"
#define SPIFFS_MAX_FILES  5

/* ── Global config (loaded at boot) ─────────────────────────────────── */

app_config_t g_config = {
    .wifi_enabled      = true,
    .ble_enabled       = true,
    .ancs_enabled      = true,
    .airplane_mode     = false,
    .brightness        = 80,
    .font_scale        = 1,
    .always_on_display = false,
    .skip_boot_screen  = false,
    .wake_on_raise_enabled = false,
    .light_sleep_enabled = true,
    .wrist_flick_enabled   = false,
    .wrist_flick_confidence_pct = 96,
    .air_pointer_enabled   = false,
    .menu_style        = 1,   /* MENU_STYLE_LIST */
    .dim_timeout_sec   = 20,
    .timezone_offset   = 8,   /* HKT */
    .region_index      = 0,   /* Hong Kong */
    .ntp_url_index     = 0,
    .cpu_protection_enabled     = true,
    .power_chips_temp_threshold = 55,
    .cpu_temp_threshold         = 65,
    .honeycomb_distinct_icons   = false,
    .honeycomb_layout_mode      = 0,
    .honeycomb_vertical_entry_animation = true,
    .honeycomb_hex_entry_animation      = true,
    .honeycomb_gap_adjust      = true,
    .charge_limit_80        = false,
    .optimized_charging_enabled = false,
    .low_power_mode         = false,
    .silent_mode            = false,
    .vibrator_strength      = 180,
    .vibrator_pattern       = 0,
    .vibrator_duration_ms   = 200,
    .last_charged_to_100_ts = 0,
    .log_enabled            = true,
    .cpu_monitor_enabled    = false,

    /* Storage / Google Drive defaults */
    .storage_gdrive_enabled       = false,
    .storage_gdrive_token         = {0},
    .storage_gdrive_folder_id     = {0},
    .storage_gdrive_upload_bmp    = true,

    .usb_msc_enabled            = true,
    .wifi_fs_enabled            = false,
    .wifi_ssid                  = {0},
    .wifi_password              = {0},
    .ota_enabled                = false,
    .ota_url                    = {0},
    .lang_code                  = "en",

    /* Watch face defaults */
    .watch_face_id              = 0,   /* WATCH_FACE_CLASSIC */
    .flux_color_top             = 0xff4800,  /* Flux default top panel */
    .flux_color_bot             = 0xe1ff00,  /* Flux default bottom panel */
    /* Analog Hands defaults: black face, silver hands, Apple-red second. */
    .hands_face_bg              = 0x000000,
    .hands_face_hour            = 0xe0e0e0,
    .hands_face_min             = 0xe0e0e0,
    .hands_face_sec             = 0xff3b30,
};

/* ── SPIFFS mount ───────────────────────────────────────────────────── */

static bool s_mounted = false;

/* Runtime-writable state lives under /spiffs/var (Linux FHS layout).
 * SPIFFS partition images do NOT persist empty directories, AND the SPIFFS
 * VFS does NOT support mkdir() at runtime (returns ENOTSUP). The /var and
 * /var/lib directories therefore exist only because the image ships a
 * placeholder file at /spiffs/var/lib/README.txt (spiffsgen creates parent
 * dirs for files). Keep that placeholder in spiffs/var/lib/. The stat()
 * below just verifies the dirs are present; mkdir() is an inert fallback. */
static void app_config_ensure_var_dirs(void)
{
    static const char *const dirs[] = {
        "/spiffs/var",
        "/spiffs/var/lib",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        struct stat st;
        if (stat(dirs[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            continue;
        }
        if (mkdir(dirs[i], 0777) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s failed: %s", dirs[i], strerror(errno));
        }
    }
}

/* Reset the honeycomb icon order to the identity permutation (0..47).
 * Called before every load so an old/corrupt config falls back to the
 * default order instead of leaving the array all-zero. */
static void app_config_icon_order_identity(void)
{
    for (int i = 0; i < 48; i++) {
        g_config.honeycomb_icon_order[i] = (uint8_t)i;
    }
}

esp_err_t app_config_init(void)
{
    ESP_LOGI(TAG, "Mounting SPIFFS partition '%s'...", SPIFFS_PARTITION);

    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE_PATH,
        .partition_label        = SPIFFS_PARTITION,
        .max_files              = SPIFFS_MAX_FILES,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Print partition info */
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(SPIFFS_PARTITION, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: %zu/%zu KB used", used / 1024, total / 1024);
    }

    s_mounted = true;

    /* Ensure the /spiffs/var hierarchy exists for runtime state files. */
    app_config_ensure_var_dirs();

    /* Icon order defaults to identity; app_config_load() overwrites it if
     * a valid saved permutation exists. */
    app_config_icon_order_identity();

    /* Load config if it exists */
    struct stat st;
    if (stat(CONFIG_FILENAME, &st) == 0) {
        ret = app_config_load();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Config load failed, using defaults");
        }
    } else {
        ESP_LOGI(TAG, "No config file found, saving defaults");
        ret = app_config_save();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save default config");
        }
    }

    ESP_LOGI(TAG, "Config: wifi=%d ble=%d ancs=%d bright=%d aod=%d menu=%d dim=%ds tz=%d region=%d ntp=%d "
             "cpu_prot=%d chips_th=%dC cpu_th=%dC",
             g_config.wifi_enabled, g_config.ble_enabled,
             g_config.ancs_enabled, g_config.brightness,
             g_config.always_on_display, g_config.menu_style,
             g_config.dim_timeout_sec, g_config.timezone_offset,
             g_config.region_index, g_config.ntp_url_index,
             g_config.cpu_protection_enabled,
             g_config.power_chips_temp_threshold,
             g_config.cpu_temp_threshold,
             g_config.honeycomb_distinct_icons);

    return ESP_OK;
}

void app_config_deinit(void)
{
    if (s_mounted) {
        esp_vfs_spiffs_unregister(SPIFFS_PARTITION);
        s_mounted = false;
        ESP_LOGI(TAG, "SPIFFS unmounted");
    }
}

/* ── JSON save/load ─────────────────────────────────────────────────── */

esp_err_t app_config_save(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "SPIFFS not mounted, cannot save");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddBoolToObject(root, "wifi_enabled",      g_config.wifi_enabled);
    cJSON_AddBoolToObject(root, "ble_enabled",       g_config.ble_enabled);
    cJSON_AddBoolToObject(root, "ancs_enabled",      g_config.ancs_enabled);
    cJSON_AddBoolToObject(root, "airplane_mode",     g_config.airplane_mode);
    cJSON_AddNumberToObject(root, "brightness",      g_config.brightness);
    cJSON_AddNumberToObject(root, "font_scale",      g_config.font_scale);
    cJSON_AddBoolToObject(root, "always_on_display", g_config.always_on_display);
    cJSON_AddBoolToObject(root, "skip_boot_screen",  g_config.skip_boot_screen);
    cJSON_AddBoolToObject(root, "wake_on_raise_enabled", g_config.wake_on_raise_enabled);
    cJSON_AddBoolToObject(root, "light_sleep_enabled",   g_config.light_sleep_enabled);
    cJSON_AddBoolToObject(root, "wrist_flick_enabled",   g_config.wrist_flick_enabled);
    cJSON_AddNumberToObject(root, "wrist_flick_confidence_pct", g_config.wrist_flick_confidence_pct);
    cJSON_AddBoolToObject(root, "air_pointer_enabled",   g_config.air_pointer_enabled);
    cJSON_AddNumberToObject(root, "menu_style",       g_config.menu_style);
    cJSON_AddNumberToObject(root, "dim_timeout_sec",  g_config.dim_timeout_sec);
    cJSON_AddNumberToObject(root, "timezone_offset",  g_config.timezone_offset);
    cJSON_AddNumberToObject(root, "region_index",     g_config.region_index);
    cJSON_AddNumberToObject(root, "ntp_url_index",    g_config.ntp_url_index);
    cJSON_AddBoolToObject(root, "cpu_protection_enabled",     g_config.cpu_protection_enabled);
    cJSON_AddNumberToObject(root, "power_chips_temp_threshold", g_config.power_chips_temp_threshold);
    cJSON_AddNumberToObject(root, "cpu_temp_threshold",         g_config.cpu_temp_threshold);
    cJSON_AddBoolToObject(root, "honeycomb_distinct_icons",     g_config.honeycomb_distinct_icons);
    cJSON_AddNumberToObject(root, "honeycomb_layout_mode",      g_config.honeycomb_layout_mode);
    cJSON_AddBoolToObject(root, "honeycomb_vertical_entry_animation", g_config.honeycomb_vertical_entry_animation);
    cJSON_AddBoolToObject(root, "honeycomb_hex_entry_animation",     g_config.honeycomb_hex_entry_animation);
    cJSON_AddBoolToObject(root, "honeycomb_gap_adjust",  g_config.honeycomb_gap_adjust);
    {
        cJSON *order = cJSON_CreateArray();
        for (int i = 0; i < 48; i++) {
            cJSON_AddItemToArray(order, cJSON_CreateNumber(g_config.honeycomb_icon_order[i]));
        }
        cJSON_AddItemToObject(root, "honeycomb_icon_order", order);
    }
    cJSON_AddBoolToObject(root, "charge_limit_80",      g_config.charge_limit_80);
    cJSON_AddBoolToObject(root, "optimized_charging_enabled", g_config.optimized_charging_enabled);
    cJSON_AddBoolToObject(root, "low_power_mode",       g_config.low_power_mode);
    cJSON_AddBoolToObject(root, "silent_mode",          g_config.silent_mode);
    cJSON_AddNumberToObject(root, "vibrator_strength",   g_config.vibrator_strength);
    cJSON_AddNumberToObject(root, "vibrator_pattern",    g_config.vibrator_pattern);
    cJSON_AddNumberToObject(root, "vibrator_duration_ms", g_config.vibrator_duration_ms);
    cJSON_AddNumberToObject(root, "last_charged_to_100_ts", g_config.last_charged_to_100_ts);
    cJSON_AddBoolToObject(root, "log_enabled",          g_config.log_enabled);
    cJSON_AddBoolToObject(root, "cpu_monitor_enabled",  g_config.cpu_monitor_enabled);

    /* Storage / Google Drive */
    cJSON_AddBoolToObject(root, "storage_gdrive_enabled",    g_config.storage_gdrive_enabled);
    cJSON_AddStringToObject(root, "storage_gdrive_token",    g_config.storage_gdrive_token);
    cJSON_AddStringToObject(root, "storage_gdrive_folder_id", g_config.storage_gdrive_folder_id);
    cJSON_AddBoolToObject(root, "storage_gdrive_upload_bmp", g_config.storage_gdrive_upload_bmp);
    cJSON_AddBoolToObject(root, "usb_msc_enabled",          g_config.usb_msc_enabled);
    cJSON_AddBoolToObject(root, "wifi_fs_enabled",          g_config.wifi_fs_enabled);
    cJSON_AddStringToObject(root, "wifi_ssid",              g_config.wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password",          g_config.wifi_password);
    cJSON_AddBoolToObject(root, "ota_enabled",              g_config.ota_enabled);
    cJSON_AddStringToObject(root, "ota_url",                g_config.ota_url);
    cJSON_AddStringToObject(root, "lang_code",              g_config.lang_code);
    cJSON_AddNumberToObject(root, "watch_face_id",          g_config.watch_face_id);
    cJSON_AddNumberToObject(root, "flux_color_top",         g_config.flux_color_top);
    cJSON_AddNumberToObject(root, "flux_color_bot",         g_config.flux_color_bot);
    cJSON_AddNumberToObject(root, "hands_face_bg",          g_config.hands_face_bg);
    cJSON_AddNumberToObject(root, "hands_face_hour",        g_config.hands_face_hour);
    cJSON_AddNumberToObject(root, "hands_face_min",         g_config.hands_face_min);
    cJSON_AddNumberToObject(root, "hands_face_sec",         g_config.hands_face_sec);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) return ESP_ERR_NO_MEM;

    /* Write to file */
    FILE *f = fopen(CONFIG_FILENAME, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing", CONFIG_FILENAME);
        cJSON_free(json_str);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    cJSON_free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Write truncated: %zu/%zu", written, len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config saved (%zu bytes)", len);
    return ESP_OK;
}

esp_err_t app_config_load(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "SPIFFS not mounted, cannot load");
        return ESP_ERR_INVALID_STATE;
    }

    /* Default the icon order to identity; overwritten below if valid. */
    app_config_icon_order_identity();

    /* Read entire file */
    FILE *f = fopen(CONFIG_FILENAME, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for reading", CONFIG_FILENAME);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 4096) {
        fclose(f);
        ESP_LOGW(TAG, "Config file size invalid: %ld", fsize);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, fsize, f);
    fclose(f);
    buf[read] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGE(TAG, "JSON parse error: %s", cJSON_GetErrorPtr());
        return ESP_FAIL;
    }

    /* Extract fields — safe getters with type checks */
    cJSON *item;

    item = cJSON_GetObjectItem(root, "wifi_enabled");
    if (cJSON_IsBool(item)) g_config.wifi_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "ble_enabled");
    if (cJSON_IsBool(item)) g_config.ble_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "ancs_enabled");

    item = cJSON_GetObjectItem(root, "airplane_mode");
    if (cJSON_IsBool(item)) g_config.airplane_mode = cJSON_IsTrue(item);
    if (cJSON_IsBool(item)) g_config.ancs_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= 10 && v <= 100) g_config.brightness = (uint8_t)v;
    }

    item = cJSON_GetObjectItem(root, "font_scale");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= 0 && v <= 2) g_config.font_scale = (uint8_t)v;
    }

    item = cJSON_GetObjectItem(root, "always_on_display");
    if (cJSON_IsBool(item)) g_config.always_on_display = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "skip_boot_screen");
    if (cJSON_IsBool(item)) g_config.skip_boot_screen = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "wake_on_raise_enabled");
    if (cJSON_IsBool(item)) g_config.wake_on_raise_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "light_sleep_enabled");
    if (cJSON_IsBool(item)) g_config.light_sleep_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "wrist_flick_enabled");
    if (cJSON_IsBool(item)) g_config.wrist_flick_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "wrist_flick_confidence_pct");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v < 20) v = 20;
        if (v > 99) v = 99;
        g_config.wrist_flick_confidence_pct = (uint8_t)v;
    }

    item = cJSON_GetObjectItem(root, "air_pointer_enabled");
    if (cJSON_IsBool(item)) g_config.air_pointer_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "menu_style");
    if (cJSON_IsNumber(item)) g_config.menu_style = (uint8_t)item->valueint;

    item = cJSON_GetObjectItem(root, "dim_timeout_sec");
    if (cJSON_IsNumber(item)) g_config.dim_timeout_sec = (uint16_t)item->valueint;

    item = cJSON_GetObjectItem(root, "timezone_offset");
    if (cJSON_IsNumber(item)) g_config.timezone_offset = (int8_t)item->valueint;

    item = cJSON_GetObjectItem(root, "region_index");
    if (cJSON_IsNumber(item)) g_config.region_index = (uint8_t)item->valueint;

    item = cJSON_GetObjectItem(root, "ntp_url_index");
    if (cJSON_IsNumber(item)) g_config.ntp_url_index = (uint8_t)item->valueint;

    item = cJSON_GetObjectItem(root, "cpu_protection_enabled");
    if (cJSON_IsBool(item)) g_config.cpu_protection_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "power_chips_temp_threshold");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= 30 && v <= 80) g_config.power_chips_temp_threshold = (uint8_t)v;
    }

    item = cJSON_GetObjectItem(root, "cpu_temp_threshold");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v >= 30 && v <= 80) g_config.cpu_temp_threshold = (uint8_t)v;
    }

    item = cJSON_GetObjectItem(root, "honeycomb_distinct_icons");
    if (cJSON_IsBool(item)) g_config.honeycomb_distinct_icons = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "honeycomb_layout_mode");
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v == 0 || v == 1) g_config.honeycomb_layout_mode = (uint8_t)v;
    }

    item = cJSON_GetObjectItem(root, "honeycomb_vertical_entry_animation");
    if (cJSON_IsBool(item)) g_config.honeycomb_vertical_entry_animation = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "honeycomb_hex_entry_animation");
    if (cJSON_IsBool(item)) g_config.honeycomb_hex_entry_animation = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "honeycomb_gap_adjust");
    if (cJSON_IsBool(item)) g_config.honeycomb_gap_adjust = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "honeycomb_icon_order");
    if (cJSON_IsArray(item) && cJSON_GetArraySize(item) == 48) {
        bool valid = true;
        bool seen[48] = { false };
        uint8_t tmp[48];
        for (int i = 0; i < 48; i++) {
            cJSON *v = cJSON_GetArrayItem(item, i);
            if (!cJSON_IsNumber(v)) { valid = false; break; }
            int id = v->valueint;
            if (id < 0 || id >= 48 || seen[id]) { valid = false; break; }
            seen[id] = true;
            tmp[i] = (uint8_t)id;
        }
        if (valid) {
            memcpy(g_config.honeycomb_icon_order, tmp, 48);
        } else {
            ESP_LOGW(TAG, "honeycomb_icon_order invalid — using identity");
        }
    }

    item = cJSON_GetObjectItem(root, "charge_limit_80");
    if (cJSON_IsBool(item)) g_config.charge_limit_80 = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "optimized_charging_enabled");
    if (cJSON_IsBool(item)) g_config.optimized_charging_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "low_power_mode");
    if (cJSON_IsBool(item)) g_config.low_power_mode = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "silent_mode");
    if (cJSON_IsBool(item)) g_config.silent_mode = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "vibrator_strength");
    if (cJSON_IsNumber(item)) g_config.vibrator_strength = (uint8_t)item->valueint;

    item = cJSON_GetObjectItem(root, "vibrator_pattern");
    if (cJSON_IsNumber(item)) g_config.vibrator_pattern = (uint8_t)item->valueint;

    item = cJSON_GetObjectItem(root, "vibrator_duration_ms");
    if (cJSON_IsNumber(item)) g_config.vibrator_duration_ms = (uint16_t)item->valueint;

    item = cJSON_GetObjectItem(root, "last_charged_to_100_ts");
    if (cJSON_IsNumber(item)) g_config.last_charged_to_100_ts = (int64_t)item->valuedouble;

    item = cJSON_GetObjectItem(root, "log_enabled");
    if (cJSON_IsBool(item)) g_config.log_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "cpu_monitor_enabled");
    if (cJSON_IsBool(item)) g_config.cpu_monitor_enabled = cJSON_IsTrue(item);

    /* Storage / Google Drive */
    item = cJSON_GetObjectItem(root, "storage_gdrive_enabled");
    if (cJSON_IsBool(item)) g_config.storage_gdrive_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "storage_gdrive_token");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(g_config.storage_gdrive_token, item->valuestring,
                APP_CFG_GDRIVE_TOKEN_MAX - 1);
        g_config.storage_gdrive_token[APP_CFG_GDRIVE_TOKEN_MAX - 1] = '\0';
    }

    item = cJSON_GetObjectItem(root, "storage_gdrive_folder_id");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(g_config.storage_gdrive_folder_id, item->valuestring,
                APP_CFG_GDRIVE_FOLDER_ID_MAX - 1);
        g_config.storage_gdrive_folder_id[APP_CFG_GDRIVE_FOLDER_ID_MAX - 1] = '\0';
    }

    item = cJSON_GetObjectItem(root, "storage_gdrive_upload_bmp");
    if (cJSON_IsBool(item)) g_config.storage_gdrive_upload_bmp = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "usb_msc_enabled");
    if (cJSON_IsBool(item)) g_config.usb_msc_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "watch_face_id");
    if (cJSON_IsNumber(item)) g_config.watch_face_id = (uint8_t)item->valueint;
    item = cJSON_GetObjectItem(root, "flux_color_top");
    if (cJSON_IsNumber(item)) g_config.flux_color_top = (uint32_t)item->valueint;
    item = cJSON_GetObjectItem(root, "flux_color_bot");
    if (cJSON_IsNumber(item)) g_config.flux_color_bot = (uint32_t)item->valueint;
    item = cJSON_GetObjectItem(root, "hands_face_bg");
    if (cJSON_IsNumber(item)) g_config.hands_face_bg = (uint32_t)item->valueint;
    item = cJSON_GetObjectItem(root, "hands_face_hour");
    if (cJSON_IsNumber(item)) g_config.hands_face_hour = (uint32_t)item->valueint;
    item = cJSON_GetObjectItem(root, "hands_face_min");
    if (cJSON_IsNumber(item)) g_config.hands_face_min = (uint32_t)item->valueint;
    item = cJSON_GetObjectItem(root, "hands_face_sec");
    if (cJSON_IsNumber(item)) g_config.hands_face_sec = (uint32_t)item->valueint;

    item = cJSON_GetObjectItem(root, "wifi_fs_enabled");
    if (cJSON_IsBool(item)) g_config.wifi_fs_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "wifi_ssid");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(g_config.wifi_ssid, item->valuestring, sizeof(g_config.wifi_ssid) - 1);
        g_config.wifi_ssid[sizeof(g_config.wifi_ssid) - 1] = '\0';
    }

    item = cJSON_GetObjectItem(root, "wifi_password");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(g_config.wifi_password, item->valuestring, sizeof(g_config.wifi_password) - 1);
        g_config.wifi_password[sizeof(g_config.wifi_password) - 1] = '\0';
    }

    item = cJSON_GetObjectItem(root, "ota_enabled");
    if (cJSON_IsBool(item)) g_config.ota_enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "ota_url");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(g_config.ota_url, item->valuestring, sizeof(g_config.ota_url) - 1);
        g_config.ota_url[sizeof(g_config.ota_url) - 1] = '\0';
    }

    item = cJSON_GetObjectItem(root, "lang_code");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(g_config.lang_code, item->valuestring, sizeof(g_config.lang_code) - 1);
        g_config.lang_code[sizeof(g_config.lang_code) - 1] = '\0';
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config loaded from flash");
    return ESP_OK;
}

/* ====================================================================
 * SD-card config backup / restore
 * ==================================================================== */

static bool app_config_sd_mounted(void)
{
    struct stat st;
    return (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode));
}

/* Create the backup directory and any missing parents on the SD card.
 * FAT (the SD card) supports mkdir() at runtime, unlike SPIFFS. */
static esp_err_t app_config_sd_make_dirs(void)
{
    static const char *const dirs[] = {
        "/sdcard",
        "/sdcard/watchos",
        "/sdcard/watchos/sys",
        APP_CFG_SD_DIR,
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (mkdir(dirs[i], 0777) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir %s failed: %s", dirs[i], strerror(errno));
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* Simple streamed file copy (1 KB buffer). */
static esp_err_t app_config_file_copy(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        ESP_LOGW(TAG, "Cannot open %s for reading", src);
        return ESP_ERR_NOT_FOUND;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        ESP_LOGE(TAG, "Cannot open %s for writing", dst);
        fclose(in);
        return ESP_FAIL;
    }
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ESP_LOGE(TAG, "Write failed copying %s", dst);
            fclose(in);
            fclose(out);
            return ESP_FAIL;
        }
    }
    fclose(in);
    if (fclose(out) != 0) {
        ESP_LOGE(TAG, "Close failed on %s", dst);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_config_backup_to_sd(void)
{
    if (!app_config_sd_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted — cannot backup config");
        return ESP_ERR_NOT_FOUND;
    }
    struct stat st;
    if (stat(CONFIG_FILENAME, &st) != 0) {
        ESP_LOGW(TAG, "No config file to back up yet");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t r = app_config_sd_make_dirs();
    if (r != ESP_OK) return r;
    r = app_config_file_copy(CONFIG_FILENAME, APP_CFG_SD_FILE);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "Config backed up to %s", APP_CFG_SD_FILE);
    }
    return r;
}

esp_err_t app_config_restore_from_sd(void)
{
    struct stat st;
    if (stat(APP_CFG_SD_FILE, &st) != 0) {
        ESP_LOGW(TAG, "No config backup at %s", APP_CFG_SD_FILE);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t r = app_config_file_copy(APP_CFG_SD_FILE, CONFIG_FILENAME);
    if (r != ESP_OK) return r;
    /* Reload so the restored settings apply live. */
    r = app_config_load();
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "Config restored from SD and reloaded");
    }
    return r;
}
