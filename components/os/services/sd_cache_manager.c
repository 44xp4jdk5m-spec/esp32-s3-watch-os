/**
 * @file sd_cache_manager.c
 * @brief SD card cache for offline screenshot queue.
 *
 * Queue format (CSV):
 *   filename,size_bytes,timestamp_epoch
 *   screenshot_20250115_143000.bmp,411234,1736926200
 *
 * The queue file is read/written line-by-line to avoid loading the
 * entire file into RAM.  For small queues (<128 entries) it's simpler
 * to parse the whole file.
 */

#include "sd_cache_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

static const char *TAG = "SD_CACHE";

static bool s_mounted = false;

/* ── Init ────────────────────────────────────────────────────────────── */

esp_err_t sd_cache_init(void)
{
    /* Check if SD card is mounted by testing the mount point */
    struct stat st;
    if (stat("/sdcard", &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "SD card not mounted");
        s_mounted = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* Create cache directory if it doesn't exist */
    struct stat dir_stat;
    if (stat(SD_CACHE_DIR, &dir_stat) != 0) {
        ESP_LOGI(TAG, "Creating cache directory: %s", SD_CACHE_DIR);
        if (mkdir(SD_CACHE_DIR, 0777) != 0) {
            ESP_LOGE(TAG, "Failed to create cache directory");
            s_mounted = false;
            return ESP_FAIL;
        }
    }

    /* Create queue file if it doesn't exist */
    struct stat q_stat;
    if (stat(SD_CACHE_QUEUE_FILE, &q_stat) != 0) {
        FILE *f = fopen(SD_CACHE_QUEUE_FILE, "w");
        if (f) fclose(f);
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD cache ready at %s", SD_CACHE_DIR);

    uint32_t pending = sd_cache_pending_count();
    if (pending > 0) {
        ESP_LOGI(TAG, "Pending uploads in cache: %" PRIu32, pending);
    }

    return ESP_OK;
}

bool sd_cache_is_mounted(void)
{
    return s_mounted;
}

/* ── Save screenshot ────────────────────────────────────────────────── */

esp_err_t sd_cache_save_screenshot(const char *file_name,
                                   const uint8_t *data,
                                   uint32_t data_size)
{
    if (!file_name || !data || !data_size) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    /* Write file to cache directory */
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/%s", SD_CACHE_DIR, file_name);

    FILE *f = fopen(file_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create %s", file_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, data_size, f);
    fclose(f);

    if (written != data_size) {
        ESP_LOGE(TAG, "Write truncated: %zu/%" PRIu32, written, data_size);
        remove(file_path);
        return ESP_FAIL;
    }

    /* Append to queue */
    f = fopen(SD_CACHE_QUEUE_FILE, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open queue file for append");
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    fprintf(f, "%s,%" PRIu32 ",%ld\n", file_name, data_size, (long)now);
    fclose(f);

    ESP_LOGI(TAG, "Cached: %s (%" PRIu32 " bytes)", file_name, data_size);
    return ESP_OK;
}

/* ── Queue helpers ─────────────────────────────────────────────────── */

uint32_t sd_cache_pending_count(void)
{
    if (!s_mounted) return 0;

    FILE *f = fopen(SD_CACHE_QUEUE_FILE, "r");
    if (!f) return 0;

    uint32_t count = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        /* Skip empty lines */
        if (buf[0] == '\n' || buf[0] == '\0') continue;
        count++;
    }
    fclose(f);
    return count;
}

esp_err_t sd_cache_get_next_pending(char **out_file_name,
                                    uint32_t *out_file_size)
{
    if (!out_file_name || !out_file_size) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(SD_CACHE_QUEUE_FILE, "r");
    if (!f) return ESP_FAIL;

    char buf[512];
    esp_err_t ret = ESP_FAIL;
    *out_file_name = NULL;

    while (fgets(buf, sizeof(buf), f)) {
        /* Trim newline */
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        if (len == 0) continue;

        /* Parse CSV: filename,size,timestamp */
        char *first_comma = strchr(buf, ',');
        if (!first_comma) continue;

        *first_comma = '\0';
        char *name = buf;
        char *size_str = first_comma + 1;

        char *second_comma = strchr(size_str, ',');
        if (second_comma) *second_comma = '\0';

        long fsize = atol(size_str);

        /* Verify file still exists on disk */
        char file_path[256];
        snprintf(file_path, sizeof(file_path), "%s/%s", SD_CACHE_DIR, name);

        struct stat st;
        if (stat(file_path, &st) == 0) {
            *out_file_name = strdup(name);
            *out_file_size = (uint32_t)fsize;
            ret = ESP_OK;
            break;
        } else {
            ESP_LOGW(TAG, "Cached file missing, removing from queue: %s", name);
            /* We can't remove inline, mark for removal at end */
        }
    }
    fclose(f);

    return ret;
}

esp_err_t sd_cache_mark_uploaded(const char *file_name)
{
    if (!file_name) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    /* Delete the cached file */
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/%s", SD_CACHE_DIR, file_name);
    remove(file_path);

    /* Rebuild queue without the uploaded entry */
    FILE *f_in = fopen(SD_CACHE_QUEUE_FILE, "r");
    if (!f_in) return ESP_FAIL;

    /* Write to a temp file */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/queue.tmp", SD_CACHE_DIR);
    FILE *f_out = fopen(tmp_path, "w");
    if (!f_out) {
        fclose(f_in);
        return ESP_FAIL;
    }

    char buf[512];
    bool found = false;
    while (fgets(buf, sizeof(buf), f_in)) {
        /* Check if this line's filename matches */
        char *comma = strchr(buf, ',');
        if (comma) {
            size_t name_len = (size_t)(comma - buf);
            if (strncmp(buf, file_name, name_len) == 0 &&
                strlen(file_name) == name_len) {
                found = true;
                continue; /* skip this entry */
            }
        }
        fputs(buf, f_out);
    }

    fclose(f_in);
    fclose(f_out);

    /* Replace queue file */
    remove(SD_CACHE_QUEUE_FILE);
    rename(tmp_path, SD_CACHE_QUEUE_FILE);

    if (found) {
        ESP_LOGI(TAG, "Removed from queue: %s", file_name);
    } else {
        ESP_LOGW(TAG, "File not found in queue: %s", file_name);
    }

    return ESP_OK;
}

esp_err_t sd_cache_read_file(const char *file_name,
                             uint8_t **out_data,
                             uint32_t *out_size)
{
    if (!file_name || !out_data || !out_size) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/%s", SD_CACHE_DIR, file_name);

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", file_path);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) {
        fclose(f);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)fsize,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "OOM: cannot allocate %ld bytes for cached file", fsize);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, (size_t)fsize, f);
    fclose(f);

    if ((long)read != fsize) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    *out_data = buf;
    *out_size = (uint32_t)fsize;
    return ESP_OK;
}
