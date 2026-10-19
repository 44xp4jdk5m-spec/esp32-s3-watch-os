/**
 * @file ota_manager.c
 * @brief OTA over WiFi implementation (download → stage → install → reboot).
 *
 * Download runs in a background FreeRTOS task (like the WiFi manager) so the
 * UI stays responsive; progress is polled by the Settings OTA page.  Install
 * shows a full-screen UI (black, big "WatchOS", the shared radial progress
 * ring), closes services that write data (WiFi file server, BLE, SD card),
 * marks the staged partition bootable, then reboots.
 */

#include "ota_manager.h"
#include "app_config.h"
#include "ui_progress_ring.h"
#include "spinner.h"
#include "wifi_file_server.h"
#include "app_ble_ancs.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "OTA";

#define OTA_CHUNK_SIZE   4096
#define OTA_DL_STACK     8192
#define OTA_INST_STACK   4096

/* ── State ──────────────────────────────────────────────────────────── */
static ota_state_t          s_state    = OTA_IDLE;
static int                  s_progress = 0;
static const esp_partition_t *s_target = NULL;   /* staged partition */

/* ── Download task ──────────────────────────────────────────────────── */

static void ota_download_fail(esp_ota_handle_t handle)
{
    if (handle) esp_ota_abort(handle);
    s_progress = 0;
    s_state    = OTA_DOWNLOAD_FAILED;
    ESP_LOGE(TAG, "Download failed");
}

static void ota_download_task(void *arg)
{
    (void)arg;

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "No OTA partition available");
        s_state = OTA_DOWNLOAD_FAILED;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Downloading to partition '%s' from %s", update->label, g_config.ota_url);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        s_state = OTA_DOWNLOAD_FAILED;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url        = g_config.ota_url,
        .timeout_ms = 10000,
        .buffer_size = OTA_CHUNK_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ota_download_fail(handle);
        vTaskDelete(NULL);
        return;
    }

    err = esp_http_client_open(client, 0);   /* GET */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        ota_download_fail(handle);
        vTaskDelete(NULL);
        return;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);

    uint8_t *buf = heap_caps_malloc(OTA_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        esp_http_client_cleanup(client);
        ota_download_fail(handle);
        vTaskDelete(NULL);
        return;
    }

    int received = 0;
    int r = 0;
    do {
        r = esp_http_client_read(client, (char *)buf, OTA_CHUNK_SIZE);
        if (r > 0) {
            if (esp_ota_write(handle, buf, (size_t)r) != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed");
                r = -2;
                break;
            }
            received += r;
            if (content_length > 0) {
                int pct = (int)((int64_t)received * 100 / content_length);
                if (pct > 100) pct = 100;
                s_progress = pct;
            }
        }
    } while (r > 0);

    heap_caps_free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (r < 0) {
        ota_download_fail(handle);
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        esp_ota_abort(handle);
        s_state = OTA_DOWNLOAD_FAILED;
        vTaskDelete(NULL);
        return;
    }

    s_target   = update;
    s_progress = 100;
    s_state    = OTA_DOWNLOAD_DONE;
    ESP_LOGI(TAG, "Firmware staged (%d bytes) — ready to install", received);
    vTaskDelete(NULL);
}

/* ── Install UI (full-screen, reuses the radial progress ring) ──────── */

static lv_obj_t   *s_inst_scr     = NULL;
static lv_obj_t   *s_inst_ring    = NULL;
static lv_obj_t   *s_inst_spinner = NULL;
static lv_timer_t *s_inst_timer   = NULL;
static volatile int  s_inst_progress  = 0;   /* ring progress 0..100 */
static volatile bool s_inst_finishing  = false; /* ring full → spinner phase */

static void ota_inst_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_inst_scr || !lv_obj_is_valid(s_inst_scr)) return;

    if (s_inst_finishing) {
        /* Ring complete → "closing services" spinner phase. */
        if (s_inst_ring && lv_obj_is_valid(s_inst_ring)) {
            lv_obj_del(s_inst_ring);
            s_inst_ring = NULL;
        }
        if (!s_inst_spinner) {
            s_inst_spinner = spinner_create(s_inst_scr, SPINNER_DEFAULT_DIAMETER);
            if (s_inst_spinner) lv_obj_center(s_inst_spinner);
        }
        return;
    }
    if (s_inst_ring) {
        ui_progress_ring_set_progress(s_inst_ring, (uint16_t)s_inst_progress);
    }
}

static void ota_install_ui_begin(void)
{
    if (s_inst_scr && lv_obj_is_valid(s_inst_scr)) return;

    s_inst_progress = 0;
    s_inst_finishing = false;
    s_inst_ring     = NULL;
    s_inst_spinner  = NULL;

    s_inst_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_inst_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_inst_scr, lv_color_hex(0x000000),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_inst_scr, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Big "WatchOS" in the centre. */
    lv_obj_t *lbl = lv_label_create(s_inst_scr);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(lbl, "WatchOS");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_48, 0);

    /* Reuse the shared radial progress ring (meets the screen edge). */
    s_inst_ring = ui_progress_ring_create(s_inst_scr, 410, 180);
    if (s_inst_ring) lv_obj_center(s_inst_ring);

    lv_screen_load(s_inst_scr);
    s_inst_timer = lv_timer_create(ota_inst_timer_cb, 50, NULL);
    ESP_LOGI(TAG, "Install UI shown");
}

/* ── Install task: close services → set boot → reboot ───────────────── */

static void ota_install_task(void *arg)
{
    (void)arg;
    if (!s_target) {
        s_state = OTA_IDLE;
        vTaskDelete(NULL);
        return;
    }

    s_inst_progress = 0;
    s_inst_finishing = false;

    /* Close services that can be mid-write, to avoid data loss. */
    s_inst_progress = 15;
    vTaskDelay(pdMS_TO_TICKS(120));
    wifi_file_server_stop();
    ESP_LOGI(TAG, "WiFi file server stopped");

    s_inst_progress = 35;
    vTaskDelay(pdMS_TO_TICKS(120));
    app_ble_ancs_stop();

    s_inst_progress = 55;
    vTaskDelay(pdMS_TO_TICKS(120));
    if (bsp_sdcard) {
        esp_err_t er = bsp_sdcard_unmount();   /* flush + unmount SD */
        ESP_LOGI(TAG, "SD unmount: %s", esp_err_to_name(er));
    }

    s_inst_progress = 75;
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_err_t err = esp_ota_set_boot_partition(s_target);
    ESP_LOGI(TAG, "Set boot partition '%s': %s", s_target->label, esp_err_to_name(err));

    s_inst_progress = 100;
    vTaskDelay(pdMS_TO_TICKS(250));

    /* Ring full → spinner while finishing up. */
    s_inst_finishing = true;
    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(TAG, "Rebooting into new firmware...");
    esp_restart();
    vTaskDelete(NULL);   /* not reached */
}

/* ── Public API ─────────────────────────────────────────────────────── */

void ota_manager_init(void)
{
    s_state    = OTA_IDLE;
    s_progress = 0;
    s_target   = NULL;
    ESP_LOGI(TAG, "OTA manager ready");
}

ota_state_t ota_manager_get_state(void) { return s_state; }
int         ota_manager_get_progress(void) { return s_progress; }

bool ota_manager_start_download(void)
{
    if (s_state == OTA_DOWNLOADING) return true;   /* already running */
    if (!g_config.ota_enabled)  { ESP_LOGW(TAG, "OTA disabled");  return false; }
    if (!g_config.ota_url[0])   { ESP_LOGW(TAG, "No OTA URL set"); return false; }

    s_state    = OTA_DOWNLOADING;
    s_progress = 0;
    if (xTaskCreatePinnedToCore(ota_download_task, "ota_dl", OTA_DL_STACK,
                                NULL, 5, NULL, 0) != pdPASS) {
        s_state = OTA_IDLE;
        return false;
    }
    return true;
}

void ota_manager_install(void)
{
    if (s_state != OTA_DOWNLOAD_DONE || !s_target) return;
    s_state = OTA_INSTALLING;

    ota_install_ui_begin();

    if (xTaskCreatePinnedToCore(ota_install_task, "ota_inst", OTA_INST_STACK,
                                NULL, 6, NULL, 0) != pdPASS) {
        esp_restart();
    }
}
