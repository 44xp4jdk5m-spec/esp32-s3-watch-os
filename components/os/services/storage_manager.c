/**
 * @file storage_manager.c
 * @brief Central orchestrator for screenshot → upload → cache workflow.
 *
 * The capture/encode pipeline runs in a separate FreeRTOS task so it
 * does not block the main loop or LVGL.
 */

#include "storage_manager.h"
#include "screen_capture.h"
#include "google_drive_uploader.h"
#include "sd_cache_manager.h"
#include "upload_notice_ui.h"
#include "app_config.h"
#include "app_events.h"
#include "app_wifi.h"
#include "hw_display.h"
#include "hw_rtc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "STORAGE_MGR";

/* ── Config keys (mirrored from app_config) ────────────────────────────
 * These fields are stored in g_config's storage_* fields.
 * See app_config.h/.c for the struct definition.
 * Note: token/folder_id are char arrays, not pointers, so
 * we use inline field checks rather than function-style macros.
 */

#define GDRIVE_ENABLED()      g_config.storage_gdrive_enabled
#define GDRIVE_TOKEN_FILLED() (g_config.storage_gdrive_token[0] != '\0')
#define GDRIVE_UPLOAD_BMP()   g_config.storage_gdrive_upload_bmp

/* ── Stats ──────────────────────────────────────────────────────────── */

static uint32_t s_capture_count = 0;

/* ── Filename generator ─────────────────────────────────────────────── */

char *storage_manager_make_filename(char *buf, size_t size)
{
    if (!buf || size < 32) return NULL;

    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) {
        /* Fallback: use monotonic counter */
        snprintf(buf, size, "screenshot_%04u.bmp", (unsigned)s_capture_count);
        return buf;
    }

    /* Convert UTC → local via config timezone */
    time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
    hw_rtc_time_t local;
    hw_rtc_from_unix(ts, &local);

    int year = 2000 + local.year;
    snprintf(buf, size, "%04d-%02d-%02d_%02d-%02d-%02d.bmp",
             year, local.month, local.day,
             local.hour, local.min, local.sec);
    return buf;
}

/* ── Screenshot task ─────────────────────────────────────────────────── */

static void screenshot_task(void *arg)
{
    (void)arg;

    char filename[64];
    if (!storage_manager_make_filename(filename, sizeof(filename))) {
        strcpy(filename, "screenshot.bmp");
    }

    ESP_LOGI(TAG, "=== Screenshot: %s ===", filename);

    /* ── Step 1: Capture screen to PSRAM ──────────────────────────── */
    screen_capture_t *cap = NULL;
    esp_err_t err = screen_capture_take(&cap);
    if (err != ESP_OK || !cap) {
        ESP_LOGE(TAG, "Screen capture failed");
        vTaskDelete(NULL);
        return;
    }

    s_capture_count++;

    /* ── Step 2: Encode as BMP in PSRAM ───────────────────────────── */
    uint8_t *bmp_data = NULL;
    uint32_t bmp_size = 0;
    err = screen_capture_encode_bmp(cap, &bmp_data, &bmp_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BMP encode failed");
        screen_capture_free(cap);
        vTaskDelete(NULL);
        return;
    }

    /* Free the raw capture — we only need the BMP now */
    screen_capture_free(cap);
    cap = NULL;

    ESP_LOGI(TAG, "BMP ready: %" PRIu32 " bytes", bmp_size);

    /* ── Step 3: Check config — is Google Drive enabled? ──────────── */
    bool uploaded = false;

    if (GDRIVE_ENABLED() && GDRIVE_TOKEN_FILLED()) {
        if (app_wifi_is_connected()) {
            ESP_LOGI(TAG, "WiFi connected — uploading to Google Drive");

            gdrive_result_t gres = google_drive_upload_file(
                filename,
                bmp_data,
                bmp_size,
                "image/bmp",
                g_config.storage_gdrive_folder_id,
                g_config.storage_gdrive_token
            );

            if (gres == GDRIVE_OK) {
                uploaded = true;
                ESP_LOGI(TAG, "Uploaded to Google Drive: %s", filename);
                hw_display_lock(200);
                upload_notice_show(UPLOAD_NOTICE_SUCCESS, "Screenshot uploaded");
                hw_display_unlock();
            } else if (gres == GDRIVE_ERR_AUTH) {
                ESP_LOGW(TAG, "Auth failed — token may be expired");
                hw_display_lock(200);
                upload_notice_show(UPLOAD_NOTICE_FAILED, "Upload auth failed");
                hw_display_unlock();
            } else {
                ESP_LOGW(TAG, "Upload failed: %s",
                         google_drive_result_str(gres));
                hw_display_lock(200);
                upload_notice_show(UPLOAD_NOTICE_FAILED, "Upload failed");
                hw_display_unlock();
            }
        } else {
            ESP_LOGI(TAG, "WiFi not connected — caching to SD");
        }
    } else {
        ESP_LOGI(TAG, "Google Drive disabled in config");
    }

    /* ── Step 4: If not uploaded, cache to SD card ────────────────── */
    if (!uploaded) {
        if (sd_cache_is_mounted()) {
            err = sd_cache_save_screenshot(filename, bmp_data, bmp_size);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Cached to SD: %s", filename);
                hw_display_lock(200);
                upload_notice_show(UPLOAD_NOTICE_CACHED, NULL);
                hw_display_unlock();
            } else {
                ESP_LOGE(TAG, "SD cache save failed");
                hw_display_lock(200);
                upload_notice_show(UPLOAD_NOTICE_FAILED, "SD cache failed");
                hw_display_unlock();
            }
        } else {
            ESP_LOGW(TAG, "SD card not mounted — screenshot lost!");
            hw_display_lock(200);
            upload_notice_show(UPLOAD_NOTICE_FAILED, "No storage available");
            hw_display_unlock();
        }
    }

    /* ── Cleanup ──────────────────────────────────────────────────── */
    if (bmp_data) {
        heap_caps_free(bmp_data);
    }

    ESP_LOGI(TAG, "=== Screenshot done ===");
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────── */

void storage_manager_init(void)
{
    /* Try to init SD card cache (non-fatal if no SD card) */
    esp_err_t err = sd_cache_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD cache not available (code %d)", err);
    } else {
        /* Process any pending uploads from previous offline sessions */
        uint32_t pending = sd_cache_pending_count();
        if (pending > 0) {
            ESP_LOGI(TAG, "Pending uploads from SD cache: %" PRIu32, pending);
            /* Will be handled by main loop when WiFi connects */
        }
    }

    ESP_LOGI(TAG, "Storage manager initialized");
}

void storage_manager_trigger_screenshot(void)
{
    ESP_LOGI(TAG, "Triggering screenshot...");

    /* Create a task for the heavy work — stack in PSRAM */
    TaskHandle_t task = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(
        screenshot_task,
        "screenshot",
        16384,      /* stack size — lv_snapshot_take needs lots of stack */
        NULL,       /* arg */
        tskIDLE_PRIORITY + 2,
        &task,
        1           /* run on APP CPU */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create screenshot task");
    }
}

void storage_manager_process_pending_uploads(uint32_t max_count)
{
    if (!GDRIVE_ENABLED() || !GDRIVE_TOKEN_FILLED() ||
        !app_wifi_is_connected() || !sd_cache_is_mounted()) {
        return;
    }

    uint32_t processed = 0;
    while (processed < max_count) {
        /* Get next pending file */
        char *file_name = NULL;
        uint32_t file_size = 0;
        esp_err_t err = sd_cache_get_next_pending(&file_name, &file_size);
        if (err != ESP_OK) {
            break; /* no more pending files */
        }

        /* Read file from SD card */
        uint8_t *file_data = NULL;
        uint32_t data_size = 0;
        err = sd_cache_read_file(file_name, &file_data, &data_size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Cannot read cached file: %s", file_name);
            sd_cache_mark_uploaded(file_name); /* remove broken entry */
            free(file_name);
            processed++;
            continue;
        }

        /* Determine MIME type from extension */
        const char *mime = "image/bmp";
        const char *ext = strrchr(file_name, '.');
        if (ext) {
            if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
                mime = "image/jpeg";
            else if (strcasecmp(ext, ".png") == 0)
                mime = "image/png";
        }

        /* Upload to Google Drive */
        gdrive_result_t gres = google_drive_upload_file(
            file_name, file_data, data_size, mime,
            g_config.storage_gdrive_folder_id,
            g_config.storage_gdrive_token
        );

        heap_caps_free(file_data);
        file_data = NULL;

        if (gres == GDRIVE_OK) {
            ESP_LOGI(TAG, "Pending upload succeeded: %s", file_name);
            sd_cache_mark_uploaded(file_name);
        } else {
            ESP_LOGW(TAG, "Pending upload failed: %s (%s)",
                     file_name, google_drive_result_str(gres));
            /* Stop processing — next call will retry from this file */
            free(file_name);
            break;
        }

        free(file_name);
        processed++;
    }

    if (processed > 0) {
        ESP_LOGI(TAG, "Processed %" PRIu32 " pending uploads", processed);
    }
}

uint32_t storage_manager_pending_count(void)
{
    return sd_cache_pending_count();
}

uint32_t storage_manager_capture_count(void)
{
    return s_capture_count;
}
