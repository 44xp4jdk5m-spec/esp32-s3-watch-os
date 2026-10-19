/**
 * @file app_wifi.c
 * @brief Direct WiFi connect → NTP time sync → RTC calibration → full cleanup.
 *
 * Also provides the Settings > WiFi scan + runtime-connect backend.
 * Respects g_config.wifi_enabled — when false, skips entirely.
 */

#include "app_wifi.h"
#include "app_config.h"
#include "hw_rtc.h"
#include "ui_boot_up.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

static const char *TAG = "APP_WIFI";

#define WIFI_CONNECT_WAIT_MS   (15 * 1000)
#define WIFI_SCAN_WAIT_MS      (10 * 1000)

/* Event bits */
#define EV_CONNECTED   BIT0   /* got IP            */
#define EV_FAILED      BIT1   /* connect failed    */
#define EV_SCAN_DONE   BIT2   /* scan finished     */
#define EV_CONN_DONE   BIT3   /* runtime connect finished */
#define EV_CONN_OK     BIT4   /* runtime connect succeeded */

/* ── State ─────────────────────────────────────────────────────────── */
static esp_netif_t        *s_sta_netif  = NULL;
static EventGroupHandle_t  s_events     = NULL;
static bool                s_wifi_up    = false;   /* wifi driver inited+started */
static bool                s_connected  = false;
static char                s_connected_ssid[33] = {0};
static bool                s_connecting = false;   /* runtime connect in progress */

/* Scan results */
static app_wifi_ap_t      s_aps[APP_WIFI_SCAN_MAX_APS];
static int                s_ap_count   = 0;
static bool                s_scan_done  = false;

static esp_err_t wifi_lazy_init(void);

/* ── Event handlers ────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* Do NOT auto-connect here. Doing so starts an auth attempt the
             * moment WiFi starts, which interrupts the Settings > WiFi scan
             * ("STA is connecting, scan are not allowed!"). Every connect
             * path (provisioning, set_enabled, connect_to) explicitly calls
             * wifi_connect_sta() -> esp_wifi_connect(). */
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
            snprintf(s_connected_ssid, sizeof s_connected_ssid, "%s", e->ssid);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            if (s_connected) {
                s_connected = false;
                ESP_LOGI(TAG, "WiFi disconnected");
            }
            xEventGroupSetBits(s_events, EV_FAILED);
            /* If a runtime connect was in progress, retry a couple times. */
            break;
        }
        case WIFI_EVENT_SCAN_DONE: {
            wifi_event_sta_scan_done_t *e = (wifi_event_sta_scan_done_t *)data;
            ESP_LOGI(TAG, "Scan done: %d APs (status %d)", e->number, e->status);
            /* Wake wifi_scan_task so it can copy the results.  Without this
             * signal it calls esp_wifi_scan_get_ap_records() while the scan
             * is still running and gets 0 records ("No networks found"). */
            xEventGroupSetBits(s_events, EV_SCAN_DONE);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_events, EV_CONNECTED | EV_CONN_OK | EV_CONN_DONE);
    }
}

/* ── Init / deinit helpers ─────────────────────────────────────────── */

static esp_err_t wifi_lazy_init(void)
{
    if (s_wifi_up) return ESP_OK;
    if (!s_events) {
        s_events = xEventGroupCreate();
        if (!s_events) return ESP_ERR_NO_MEM;
    }
    if (!s_sta_netif) {
        /* hw_init_all() already initialises the TCP/IP stack + default event
         * loop at boot — both return ESP_ERR_INVALID_STATE if they exist,
         * which is fine here (idempotent). */
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) return ESP_ERR_NO_MEM;
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &wifi_event_handler, NULL));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_up = true;
    return ESP_OK;
}

static void wifi_full_deinit(void)
{
    if (!s_wifi_up) return;
    if (s_connected) {
        esp_wifi_disconnect();
        s_connected = false;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_up = false;
    memset(s_connected_ssid, 0, sizeof s_connected_ssid);
}

/* Tear the WiFi driver back down when it is no longer needed (e.g. leaving
 * the Settings > WiFi page).  Keeps an active connection alive — only frees
 * the driver when nothing is connected, so a scan alone doesn't leave the
 * system with almost no internal RAM/DMA (DMA free drops to ~0 B). */
void app_wifi_idle_deinit(void)
{
    if (s_connected) return;
    wifi_full_deinit();
}

static esp_err_t wifi_connect_sta(const char *ssid, const char *password)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid - 1);
    if (password && password[0]) {
        strncpy((char *)cfg.sta.password, password, sizeof cfg.sta.password - 1);
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable    = true;
    cfg.sta.pmf_cfg.required   = false;
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    xEventGroupClearBits(s_events, EV_CONNECTED | EV_FAILED);
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

/* ── SNTP sync → RTC ───────────────────────────────────────────────── */

static bool ntp_sync_rtc(void)
{
    /* SNTP needs WiFi up with an IP — poll until we have it. */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_init();

    time_t now = 0;
    for (int i = 0; i < 20; i++) {   /* up to ~10 s for NTP */
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        if (now > 1600000000) break;   /* 2020-09+ : got a real time */
    }
    esp_sntp_stop();

    if (now > 1600000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        hw_rtc_time_t rt = {
            .sec   = (uint8_t)tm.tm_sec,
            .min   = (uint8_t)tm.tm_min,
            .hour  = (uint8_t)tm.tm_hour,
            .day   = (uint8_t)tm.tm_mday,
            .wday  = (uint8_t)(tm.tm_wday + 1),   /* PCF85063: 1=Mon..7=Sun */
            .month = (uint8_t)(tm.tm_mon + 1),
            .year  = (uint8_t)((tm.tm_year + 1900) - 2000),
        };
        esp_err_t r = hw_rtc_set_time(&rt);
        ESP_LOGI(TAG, "NTP time synced -> RTC set (%04d-%02d-%02d %02d:%02d:%02d) ret=%s",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, esp_err_to_name(r));
        return (r == ESP_OK);
    }
    ESP_LOGW(TAG, "NTP sync failed (no time yet)");
    return false;
}

/* ── Public API ────────────────────────────────────────────────────── */

void app_wifi_start_provisioning(void)
{
    if (!g_config.wifi_enabled) {
        ESP_LOGI(TAG, "WiFi disabled in config — skipping");
        /* Don't jump the ring to 100% here: when there is nothing to wait
         * for, let the boot ring play out its fill animation on the ramp
         * timer (ui_boot_up_finish() completes it after the minimum boot
         * animation time). */
        return;
    }
    if (!g_config.wifi_ssid[0]) {
        ESP_LOGI(TAG, "No WiFi SSID configured — skipping");
        return;
    }
    ESP_LOGI(TAG, "Provisioning: connecting to '%s' ...", g_config.wifi_ssid);
    if (wifi_lazy_init() != ESP_OK) return;
    if (wifi_connect_sta(g_config.wifi_ssid, g_config.wifi_password) != ESP_OK) return;
    ui_boot_up_bump_progress(45);   /* driver up, auth/assoc in progress */

    EventBits_t bits = xEventGroupWaitBits(s_events, EV_CONNECTED | EV_FAILED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_WAIT_MS));
    if (bits & EV_CONNECTED) {
        ui_boot_up_bump_progress(85);   /* got IP — NTP sync next */
        ntp_sync_rtc();
    } else {
        ESP_LOGW(TAG, "Provisioning connect timed out");
        ui_boot_up_bump_progress(90);
    }

    /* Full cleanup: we only needed WiFi for the time sync. */
    wifi_full_deinit();
}

bool app_wifi_is_connected(void)
{
    return s_connected;
}

void app_wifi_set_enabled(bool enable)
{
    if (enable) {
        if (wifi_lazy_init() != ESP_OK) return;
        if (g_config.wifi_ssid[0]) {
            wifi_connect_sta(g_config.wifi_ssid, g_config.wifi_password);
        }
    } else {
        wifi_full_deinit();
    }
}

/* ── Runtime scan ──────────────────────────────────────────────────── */

/* Background task: wait for the async scan to finish and copy results. */
static void wifi_scan_task(void *arg)
{
    (void)arg;
    /* Wait for WIFI_EVENT_SCAN_DONE (set by wifi_event_handler) before
     * pulling the AP list — esp_wifi_scan_get_ap_records() called while the
     * scan is still running returns 0 records ("No networks found"). */
    xEventGroupWaitBits(s_events, EV_SCAN_DONE, pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(WIFI_SCAN_WAIT_MS));

    wifi_ap_record_t recs[APP_WIFI_SCAN_MAX_APS];
    uint16_t n = APP_WIFI_SCAN_MAX_APS;
    esp_err_t r = esp_wifi_scan_get_ap_records(&n, recs);
    if (r != ESP_OK) n = 0;
    s_ap_count = (n > APP_WIFI_SCAN_MAX_APS) ? APP_WIFI_SCAN_MAX_APS : (int)n;
    for (int i = 0; i < s_ap_count; i++) {
        snprintf(s_aps[i].ssid, sizeof s_aps[i].ssid, "%s", recs[i].ssid);
        s_aps[i].rssi     = recs[i].rssi;
        s_aps[i].authmode = (uint8_t)recs[i].authmode;
    }
    s_scan_done = true;
    vTaskDelete(NULL);
}

bool app_wifi_scan_begin(void)
{
    if (wifi_lazy_init() != ESP_OK) return false;
    s_ap_count = 0;
    s_scan_done = false;
    xEventGroupClearBits(s_events, EV_SCAN_DONE);

    wifi_scan_config_t scan = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        /* Leave scan_time as the default: ESP-IDF warns (and may reject)
         * custom active-scan times while BLE is enabled. */
    };
    if (esp_wifi_scan_start(&scan, false) != ESP_OK) return false;

    if (xTaskCreatePinnedToCore(wifi_scan_task, "wifi_scan", 4096, NULL, 5, NULL, 0) != pdPASS) {
        return false;
    }
    return true;
}

bool app_wifi_scan_is_done(void) { return s_scan_done; }
int  app_wifi_scan_get_count(void) { return s_ap_count; }

bool app_wifi_scan_get(int idx, app_wifi_ap_t *out)
{
    if (idx < 0 || idx >= s_ap_count || !out) return false;
    *out = s_aps[idx];
    return true;
}

/* ── Runtime connect (Settings > WiFi list) ────────────────────────── */

/* Background task: wait for the runtime connect to finish. */
static void wifi_conn_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_events, EV_CONN_DONE | EV_FAILED, pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(WIFI_CONNECT_WAIT_MS));
    s_connecting = false;
    xEventGroupSetBits(s_events, EV_CONN_DONE);
    vTaskDelete(NULL);
}

void app_wifi_connect_to(const char *ssid, const char *password)
{
    if (!ssid) return;
    snprintf(g_config.wifi_ssid, sizeof g_config.wifi_ssid, "%s", ssid);
    snprintf(g_config.wifi_password, sizeof g_config.wifi_password,
             password ? password : "");
    app_config_save();

    s_connecting = true;
    /* wifi_lazy_init() creates s_events, so clear bits only AFTER it —
     * otherwise xEventGroupClearBits(NULL) asserts if WiFi was never inited. */
    if (wifi_lazy_init() != ESP_OK) { s_connecting = false; return; }
    xEventGroupClearBits(s_events, EV_CONN_DONE | EV_CONN_OK);
    if (wifi_connect_sta(g_config.wifi_ssid, g_config.wifi_password) != ESP_OK) {
        s_connecting = false;
        return;
    }

    /* Async wait in a task so the UI can poll. */
    if (xTaskCreatePinnedToCore(wifi_conn_task, "wifi_conn", 3072, NULL, 5, NULL, 0) != pdPASS) {
        s_connecting = false;
    }
}

bool app_wifi_is_connecting(void) { return s_connecting; }

bool app_wifi_connect_is_done(void)
{
    /* s_events is NULL until WiFi is first initialized (boot provisioning
     * may be skipped when WiFi is disabled / no SSID configured). */
    if (!s_events) return true;   /* nothing ever started → nothing pending */
    return (xEventGroupGetBits(s_events) & EV_CONN_DONE) != 0;
}

bool app_wifi_connect_ok(void)
{
    if (!s_events) return false;  /* never initialized → not connected */
    return (xEventGroupGetBits(s_events) & EV_CONN_OK) != 0;
}
const char *app_wifi_get_connected_ssid(void) { return s_connected_ssid; }
