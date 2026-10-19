/**
 * @file app_ble_ancs.c
 * @brief Application-level BLE ANCS wrapper — BLE init, GATT client, ANCS discovery.
 *
 * Architecture:
 *   ESP32 advertises as BLE peripheral → iPhone connects as central
 *   → ESP32 discovers iPhone's ANCS service (GATT client)
 *   → subscribes to Notification Source & Data Source
 *   → on notification, requests detailed attributes via Control Point
 *
 * Based on ESP-IDF v5.5.2 ble_ancs example (uses Bluedroid, not NimBLE).
 * Key difference from Arduino BLE: all state is event-driven with callbacks.
 */

#include "app_ble_ancs.h"
#include "ble_ancs.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "APP_ANCS";

/* ── Configuration ────────────────────────────────────────────────── */

#define PROFILE_A_APP_ID             0
#define PROFILE_NUM                  1
#define ADV_CONFIG_FLAG              (1 << 0)
#define SCAN_RSP_CONFIG_FLAG         (1 << 1)
#define INVALID_HANDLE               0
#define ANCS_CMD_BUFFER_MAX_SIZE     600
#define ANCS_DEVICE_NAME_DEFAULT     "ESP32-Watch"

/* ── Global state ─────────────────────────────────────────────────── */

bool              g_ancs_connected           = false;
bool              g_ancs_ready               = false;
char              g_ancs_passkey[8]          = {0};
ancs_notification_t g_latest_notification    = {0};
bool              g_new_notification_pending  = false;

/* ── Internal state ───────────────────────────────────────────────── */

static bool      g_paired                   = false;
static bool      g_cccd_ns_ok               = false;
static bool      g_cccd_ds_ok               = false;

/* ── ANCS UUIDs ───────────────────────────────────────────────────── */

static const uint8_t Apple_NC_UUID[16] = {
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79
};

static const uint8_t notification_source_uuid[16] = {
    0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c,
    0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f
};

static const uint8_t control_point_uuid[16] = {
    0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98,
    0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69
};

static const uint8_t data_source_uuid[16] = {
    0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe,
    0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22
};

/* ── HID service UUID (makes iOS show proper ANCS pairing UI) ─────── */

static uint8_t hidd_service_uuid128[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00
};

/* ── Advertising data ─────────────────────────────────────────────── */

static esp_ble_adv_data_t adv_config = {
    .set_scan_rsp    = false,
    .include_txpower = false,
    .min_interval    = 0x0006,
    .max_interval    = 0x0010,
    .appearance      = ESP_BLE_APPEARANCE_GENERIC_WATCH,
    .service_uuid_len = sizeof(hidd_service_uuid128),
    .p_service_uuid   = hidd_service_uuid128,
    .flag             = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t scan_rsp_config = {
    .set_scan_rsp    = true,
    .include_name    = true,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x100,
    .adv_int_max       = 0x100,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_RPA_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ── GATT profile instance ────────────────────────────────────────── */

typedef struct {
    esp_gattc_cb_t gattc_cb;
    uint16_t       gattc_if;
    uint16_t       app_id;
    uint16_t       conn_id;
    uint16_t       service_start_handle;
    uint16_t       service_end_handle;
    uint16_t       notification_source_handle;
    uint16_t       data_source_handle;
    uint16_t       control_point_handle;
    esp_bd_addr_t  remote_bda;
    uint16_t       MTU_size;
} gattc_profile_t;

static gattc_profile_t gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {
        .gattc_cb = NULL,  /* set later */
        .gattc_if = ESP_GATT_IF_NONE,
    },
};

/* ── Data source reassembly ───────────────────────────────────────── */
/* 1024-byte reassembly buffer — allocate in PSRAM to save internal DRAM.
 * Falls back to internal if PSRAM unavailable. */
#define DATA_BUFFER_SIZE  1024
static uint8_t *data_buffer_ptr = NULL;
static uint16_t data_buffer_len = 0;

static esp_timer_handle_t periodic_timer = NULL;

/* ── Attribute request template ───────────────────────────────────── */

static esp_noti_attr_list_t p_attr[8] = {
    [attr_appidentifier_index]   = { NotificationAttributeIDAppIdentifier,       0 },
    [attr_title_index]           = { NotificationAttributeIDTitle,               0xFFFF },
    [attr_subtitle_index]        = { NotificationAttributeIDSubtitle,            0xFFFF },
    [attr_message_index]         = { NotificationAttributeIDMessage,             0xFFFF },
    [attr_messagesize_index]     = { NotificationAttributeIDMessageSize,         0 },
    [attr_date_index]            = { NotificationAttributeIDDate,                0 },
    [attr_positiveactionlabel_index] = { NotificationAttributeIDPositiveActionLabel, 0 },
    [attr_negativeactionlabel_index] = { NotificationAttributeIDNegativeActionLabel, 0 },
};

/* ── Forward declarations ─────────────────────────────────────────── */

static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                         esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param);
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param);
static void esp_gattc_cb(esp_gattc_cb_event_t event,
                          esp_gatt_if_t gattc_if,
                          esp_ble_gattc_cb_param_t *param);

/* ── Control Point write helpers ──────────────────────────────────── */

static void ancs_get_notification_attributes(uint8_t *notificationUID,
                                              uint8_t num_attr,
                                              esp_noti_attr_list_t *p_attr_list)
{
    /* Static — BTC task is single-threaded, avoids 600-byte stack alloc */
    static uint8_t cmd[ANCS_CMD_BUFFER_MAX_SIZE];
    uint32_t index = 0;
    memset(cmd, 0, sizeof(cmd));

    cmd[0] = CommandIDGetNotificationAttributes;
    index++;
    memcpy(&cmd[index], notificationUID, ESP_NOTIFICATIONUID_LEN);
    index += ESP_NOTIFICATIONUID_LEN;

    while (num_attr > 0) {
        if (index >= ANCS_CMD_BUFFER_MAX_SIZE) {
            ESP_LOGE(TAG, "Command buffer overflow");
            return;
        }
        cmd[index++] = p_attr_list->noti_attribute_id;
        if (p_attr_list->attribute_len > 0) {
            if ((index + 2) > ANCS_CMD_BUFFER_MAX_SIZE) {
                ESP_LOGE(TAG, "Command buffer overflow");
                return;
            }
            cmd[index++] = p_attr_list->attribute_len & 0xFF;
            cmd[index++] = (p_attr_list->attribute_len >> 8) & 0xFF;
        }
        p_attr_list++;
        num_attr--;
    }

    esp_ble_gattc_write_char(gl_profile_tab[PROFILE_A_APP_ID].gattc_if,
                              gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                              gl_profile_tab[PROFILE_A_APP_ID].control_point_handle,
                              index, cmd,
                              ESP_GATT_WRITE_TYPE_RSP,
                              ESP_GATT_AUTH_REQ_NONE);
}

/* ── ANCS notification callback (from ble_ancs.c) ─────────────────── */

static void on_ancs_event(const ancs_notification_t *notif)
{
    if (!notif) return;

    if (notif->event_id == EventIDNotificationAdded &&
        notif->category_id == CategoryIDIncomingCall) {
        /* Auto-reject incoming calls (watch can't answer) */
        ESP_LOGI(TAG, "Incoming call — auto reject, UID %" PRIu32, notif->uid);
        uint8_t uid_bytes[4];
        uid_bytes[0] = notif->uid & 0xFF;
        uid_bytes[1] = (notif->uid >> 8) & 0xFF;
        uid_bytes[2] = (notif->uid >> 16) & 0xFF;
        uid_bytes[3] = (notif->uid >> 24) & 0xFF;

        uint8_t buf[ANCS_CMD_BUFFER_MAX_SIZE] = {0};
        buf[0] = CommandIDPerformNotificationAction;
        buf[1] = uid_bytes[0]; buf[2] = uid_bytes[1];
        buf[3] = uid_bytes[2]; buf[4] = uid_bytes[3];
        buf[5] = ActionIDNegative;
        esp_ble_gattc_write_char(gl_profile_tab[PROFILE_A_APP_ID].gattc_if,
                                  gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                  gl_profile_tab[PROFILE_A_APP_ID].control_point_handle,
                                  6, buf,
                                  ESP_GATT_WRITE_TYPE_RSP,
                                  ESP_GATT_AUTH_REQ_NONE);
    } else if (notif->event_id == EventIDNotificationAdded) {
        /* Request detailed attributes for new notifications */
        ESP_LOGI(TAG, "New notification UID %" PRIu32 " — requesting details",
                 notif->uid);
        ancs_get_notification_attributes(
            (uint8_t *)&notif->uid,
            sizeof(p_attr) / sizeof(esp_noti_attr_list_t),
            p_attr);
    } else if (notif->event_id == EventIDNotificationRemoved) {
        ESP_LOGI(TAG, "Notification removed UID %" PRIu32, notif->uid);
    }
}

static void on_ancs_notification(const ancs_notification_t *notif)
{
    if (!notif) return;

    ESP_LOGI(TAG, "=== Notification ===");
    ESP_LOGI(TAG, "  UID:     %" PRIu32, notif->uid);
    ESP_LOGI(TAG, "  App:     %s", notif->has_app_id ? notif->app_id : "(none)");
    ESP_LOGI(TAG, "  Title:   %s", notif->has_title ? notif->title : "(none)");
    ESP_LOGI(TAG, "  Message: %s", notif->has_message ? notif->message : "(none)");

    /* Thread-safe copy to global state — main loop picks this up */
    memcpy(&g_latest_notification, notif, sizeof(ancs_notification_t));
    g_new_notification_pending = true;
}

/* ── Timer callback (data source reassembly) ──────────────────────── */

static void periodic_timer_callback(void *arg)
{
    esp_timer_stop(periodic_timer);
    if (data_buffer_len > 0 && data_buffer_ptr) {
        esp_receive_apple_data_source(data_buffer_ptr, data_buffer_len);
        memset(data_buffer_ptr, 0, data_buffer_len);
        data_buffer_len = 0;
    }
}

/* ── GAP event handler ────────────────────────────────────────────── */

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    static uint8_t adv_config_done = 0;

    switch (event) {
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= ~SCAN_RSP_CONFIG_FLAG;
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= ~ADV_CONFIG_FLAG;
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed: 0x%x",
                     param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "Advertising started — waiting for iPhone...");
        }
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        /* Show passkey to user for pairing */
        snprintf(g_ancs_passkey, sizeof(g_ancs_passkey),
                 "%06" PRIu32, param->ble_security.key_notif.passkey);
        ESP_LOGI(TAG, "Passkey: %s — enter on iPhone", g_ancs_passkey);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
        ESP_LOGI(TAG, "NC confirm, passkey: %" PRIu32,
                 param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "Pairing %s",
                 param->ble_security.auth_cmpl.success ? "success" : "fail");
        if (!param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "Fail reason: 0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
            g_paired = false;
            break;
        }
        g_paired = true;
        /* Now that pairing is done, proceed with service discovery.
         * Doing this before pairing causes CCCD writes to fail with
         * insufficient authentication (0x5). */
        if (g_ancs_connected) {
            esp_ble_gattc_send_mtu_req(gl_profile_tab[PROFILE_A_APP_ID].gattc_if,
                                        gl_profile_tab[PROFILE_A_APP_ID].conn_id);
            ESP_LOGI(TAG, "Pairing OK — starting service discovery");
        }
        break;

    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
        if (param->local_privacy_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Local privacy config failed: 0x%x",
                     param->local_privacy_cmpl.status);
            break;
        }
        esp_ble_gap_config_adv_data(&adv_config);
        adv_config_done |= ADV_CONFIG_FLAG;
        esp_ble_gap_config_adv_data(&scan_rsp_config);
        adv_config_done |= SCAN_RSP_CONFIG_FLAG;
        break;

    default:
        break;
    }
}

/* ── GATT client event handler ────────────────────────────────────── */

static void gattc_profile_event_handler(esp_gattc_cb_event_t event,
                                         esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param)
{
    static bool get_service = false;
    static esp_gattc_char_elem_t  *char_elem_result  = NULL;
    static esp_gattc_descr_elem_t *descr_elem_result = NULL;
    static esp_bt_uuid_t apple_nc_uuid = { .len = ESP_UUID_LEN_128 };

    switch (event) {

    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "GATT client registered");
        esp_ble_gap_config_local_privacy(true);
        break;

    case ESP_GATTC_CONNECT_EVT:
        memcpy(gl_profile_tab[PROFILE_A_APP_ID].remote_bda,
               param->connect.remote_bda, ESP_BD_ADDR_LEN);
        esp_ble_gatt_creat_conn_params_t open_params = {0};
        memcpy(&open_params.remote_bda,
               gl_profile_tab[PROFILE_A_APP_ID].remote_bda, ESP_BD_ADDR_LEN);
        open_params.remote_addr_type = BLE_ADDR_TYPE_RANDOM;
        open_params.own_addr_type    = BLE_ADDR_TYPE_RPA_PUBLIC;
        open_params.is_direct        = true;
        open_params.is_aux           = false;
        open_params.phy_mask         = 0x0;
        esp_ble_gattc_enh_open(gl_profile_tab[PROFILE_A_APP_ID].gattc_if,
                                &open_params);
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Open failed: 0x%x", param->open.status);
            break;
        }
        ESP_LOGI(TAG, "iPhone connected");
        g_ancs_connected = true;
        gl_profile_tab[PROFILE_A_APP_ID].conn_id = param->open.conn_id;
        /* Request pairing first — service discovery deferred to
         * ESP_GAP_BLE_AUTH_CMPL_EVT after pairing succeeds. */
        esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "MTU config failed: 0x%x", param->cfg_mtu.status);
        }
        ESP_LOGI(TAG, "MTU: %d", param->cfg_mtu.mtu);
        gl_profile_tab[PROFILE_A_APP_ID].MTU_size = param->cfg_mtu.mtu;
        /* Search for ANCS */
        memcpy(apple_nc_uuid.uuid.uuid128, Apple_NC_UUID, 16);
        esp_ble_gattc_search_service(gattc_if,
                                      gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                      &apple_nc_uuid);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128) {
            gl_profile_tab[PROFILE_A_APP_ID].service_start_handle =
                param->search_res.start_handle;
            gl_profile_tab[PROFILE_A_APP_ID].service_end_handle =
                param->search_res.end_handle;
            get_service = true;
            ESP_LOGI(TAG, "ANCS service found");
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Service search failed: 0x%x",
                     param->search_cmpl.status);
            break;
        }
        ESP_LOGI(TAG, "Service search complete");

        if (get_service) {
            uint16_t count = 0;
            esp_gatt_status_t ret = esp_ble_gattc_get_attr_count(
                gattc_if,
                gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                ESP_GATT_DB_CHARACTERISTIC,
                gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                INVALID_HANDLE, &count);

            if (ret != ESP_GATT_OK) {
                ESP_LOGE(TAG, "get_attr_count error");
                break;
            }

            if (count > 0) {
                char_elem_result = heap_caps_malloc(
                    sizeof(esp_gattc_char_elem_t) * count,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!char_elem_result) {
                    char_elem_result = malloc(sizeof(esp_gattc_char_elem_t) * count);
                }
                if (!char_elem_result) {
                    ESP_LOGE(TAG, "malloc failed");
                    break;
                }
                memset(char_elem_result, 0xFF,
                       sizeof(esp_gattc_char_elem_t) * count);
                ret = esp_ble_gattc_get_all_char(
                    gattc_if,
                    gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                    gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                    gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                    char_elem_result, &count, 0);

                if (ret != ESP_GATT_OK) {
                    ESP_LOGE(TAG, "get_all_char error");
                    free(char_elem_result);
                    char_elem_result = NULL;
                    break;
                }

                for (int i = 0; i < count; i++) {
                    if (char_elem_result[i].uuid.len != ESP_UUID_LEN_128)
                        continue;

                    uint8_t *uuid = char_elem_result[i].uuid.uuid.uuid128;

                    if ((char_elem_result[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) &&
                        memcmp(uuid, notification_source_uuid, 16) == 0) {
                        gl_profile_tab[PROFILE_A_APP_ID].notification_source_handle =
                            char_elem_result[i].char_handle;
                        esp_ble_gattc_register_for_notify(
                            gattc_if,
                            gl_profile_tab[PROFILE_A_APP_ID].remote_bda,
                            char_elem_result[i].char_handle);
                        ESP_LOGI(TAG, "Found Notification Source");

                    } else if ((char_elem_result[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) &&
                               memcmp(uuid, data_source_uuid, 16) == 0) {
                        gl_profile_tab[PROFILE_A_APP_ID].data_source_handle =
                            char_elem_result[i].char_handle;
                        esp_ble_gattc_register_for_notify(
                            gattc_if,
                            gl_profile_tab[PROFILE_A_APP_ID].remote_bda,
                            char_elem_result[i].char_handle);
                        ESP_LOGI(TAG, "Found Data Source");

                    } else if ((char_elem_result[i].properties & ESP_GATT_CHAR_PROP_BIT_WRITE) &&
                               memcmp(uuid, control_point_uuid, 16) == 0) {
                        gl_profile_tab[PROFILE_A_APP_ID].control_point_handle =
                            char_elem_result[i].char_handle;
                        ESP_LOGI(TAG, "Found Control Point");
                    }
                }

                free(char_elem_result);
                char_elem_result = NULL;
            }
        } else {
            ESP_LOGW(TAG, "No ANCS service found on iPhone");
        }
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Register notify failed: 0x%x",
                     param->reg_for_notify.status);
            break;
        }

        /* Enable CCCD to receive notifications */
        uint16_t descr_count = 0;
        esp_gatt_status_t ret = esp_ble_gattc_get_attr_count(
            gattc_if,
            gl_profile_tab[PROFILE_A_APP_ID].conn_id,
            ESP_GATT_DB_DESCRIPTOR,
            gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
            gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
            param->reg_for_notify.handle, &descr_count);

        if (ret != ESP_GATT_OK || descr_count == 0) break;

        descr_elem_result = heap_caps_malloc(
            sizeof(esp_gattc_descr_elem_t) * descr_count,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!descr_elem_result) {
            descr_elem_result = malloc(sizeof(esp_gattc_descr_elem_t) * descr_count);
        }
        if (!descr_elem_result) break;

        ret = esp_ble_gattc_get_all_descr(
            gattc_if, gl_profile_tab[PROFILE_A_APP_ID].conn_id,
            param->reg_for_notify.handle,
            descr_elem_result, &descr_count, 0);

        if (ret != ESP_GATT_OK) {
            free(descr_elem_result);
            descr_elem_result = NULL;
            break;
        }

        uint8_t notify_en[2] = {0x01, 0x00};
        for (int i = 0; i < descr_count; i++) {
            if (descr_elem_result[i].uuid.len == ESP_UUID_LEN_16 &&
                descr_elem_result[i].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
                esp_ble_gattc_write_char_descr(
                    gattc_if, gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                    descr_elem_result[i].handle,
                    sizeof(notify_en), notify_en,
                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                break;
            }
        }

        free(descr_elem_result);
        descr_elem_result = NULL;

        /* ANCS ready status will be set in ESP_GATTC_WRITE_DESCR_EVT
         * once both CCCDs are confirmed — avoids the duplicate log. */
        break;
    }

    case ESP_GATTC_NOTIFY_EVT:
        if (param->notify.handle ==
            gl_profile_tab[PROFILE_A_APP_ID].notification_source_handle) {
            /* Notification Source: event (added/modified/removed) */
            esp_receive_apple_notification_source(param->notify.value,
                                                   param->notify.value_len);
        } else if (param->notify.handle ==
                   gl_profile_tab[PROFILE_A_APP_ID].data_source_handle) {
            /* Data Source: may be fragmented across multiple packets */
            if (!data_buffer_ptr) break;
            if ((data_buffer_len + param->notify.value_len) > DATA_BUFFER_SIZE) {
                ESP_LOGE(TAG, "Data source buffer overflow");
                memset(data_buffer_ptr, 0, DATA_BUFFER_SIZE);
                data_buffer_len = 0;
                break;
            }
            memcpy(&data_buffer_ptr[data_buffer_len],
                   param->notify.value, param->notify.value_len);
            data_buffer_len += param->notify.value_len;

            if (param->notify.value_len ==
                (gl_profile_tab[PROFILE_A_APP_ID].MTU_size - 3)) {
                /* More packets coming — wait 500ms */
                esp_timer_start_periodic(periodic_timer, 500000);
            } else {
                /* Last packet — process immediately */
                esp_timer_stop(periodic_timer);
                esp_receive_apple_data_source(data_buffer_ptr,
                                               data_buffer_len);
                memset(data_buffer_ptr, 0, data_buffer_len);
                data_buffer_len = 0;
            }
        }
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Write descr failed: 0x%x", param->write.status);
            break;
        }
        /* Track which CCCD was successfully written */
        if (param->write.handle == gl_profile_tab[PROFILE_A_APP_ID].notification_source_handle) {
            /* Actually the write_descr handle is the CCCD handle, not the char handle.
             * Use a simpler heuristic: first success = NS, second = DS. */
            if (!g_cccd_ns_ok) {
                g_cccd_ns_ok = true;
                ESP_LOGI(TAG, "Notification Source CCCD enabled");
            } else if (!g_cccd_ds_ok) {
                g_cccd_ds_ok = true;
                ESP_LOGI(TAG, "Data Source CCCD enabled");
            }
        } else {
            /* Generic CCCD confirmation — track by order */
            if (!g_cccd_ns_ok) {
                g_cccd_ns_ok = true;
            } else if (!g_cccd_ds_ok) {
                g_cccd_ds_ok = true;
            }
        }
        /* ANCS is ready when both CCCDs are confirmed */
        if (g_cccd_ns_ok && g_cccd_ds_ok && !g_ancs_ready) {
            g_ancs_ready = true;
            ESP_LOGI(TAG, "ANCS ready — receiving notifications");
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Write char error: %s",
                     Errcode_to_String(param->write.status));
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "iPhone disconnected, reason: 0x%x",
                 param->disconnect.reason);
        g_ancs_connected = false;
        g_ancs_ready = false;
        g_paired = false;
        g_cccd_ns_ok = false;
        g_cccd_ds_ok = false;
        get_service = false;
        /* Reset handles */
        gl_profile_tab[PROFILE_A_APP_ID].notification_source_handle = INVALID_HANDLE;
        gl_profile_tab[PROFILE_A_APP_ID].data_source_handle = INVALID_HANDLE;
        gl_profile_tab[PROFILE_A_APP_ID].control_point_handle = INVALID_HANDLE;
        /* Re-start advertising so iPhone can reconnect */
        esp_ble_gap_start_advertising(&adv_params);
        break;

    case ESP_GATTC_SRVC_CHG_EVT:
        ESP_LOGI(TAG, "Service changed");
        break;

    default:
        break;
    }
}

/* ── Top-level GATT callback (ESP-IDF dispatches to our handler) ──── */

static void esp_gattc_cb(esp_gattc_cb_event_t event,
                          esp_gatt_if_t gattc_if,
                          esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gattc_if = gattc_if;
        } else {
            ESP_LOGE(TAG, "Reg app failed, app_id %04x, status %d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }

    for (int i = 0; i < PROFILE_NUM; i++) {
        if (gattc_if == ESP_GATT_IF_NONE ||
            gattc_if == gl_profile_tab[i].gattc_if) {
            if (gl_profile_tab[i].gattc_cb) {
                gl_profile_tab[i].gattc_cb(event, gattc_if, param);
            }
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t app_ble_ancs_init(const char *device_name)
{
    esp_err_t ret;

    if (!device_name) {
        device_name = ANCS_DEVICE_NAME_DEFAULT;
    }

    /* Ensure NVS is initialized (needed for BT bonding storage).
     * If already initialized by WiFi init, this is a harmless no-op. */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS init warning: %s", esp_err_to_name(ret));
        /* Non-fatal — BT may still work without persistent bonding */
    }

    /* Register ANCS parsing callbacks */
    ancs_register_event_callback(on_ancs_event);
    ancs_register_notification_callback(on_ancs_notification);

    /* Set profile callback */
    gl_profile_tab[PROFILE_A_APP_ID].gattc_cb = gattc_profile_event_handler;

    /* Release classic BT memory (we only need BLE) */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    /* Init BLE controller */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BLE controller initialized");

    /* Init Bluedroid */
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register GAP and GATTC callbacks */
    ret = esp_ble_gattc_register_callback(esp_gattc_cb);
    if (ret) {
        ESP_LOGE(TAG, "GATTC register error: %x", ret);
        return ret;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "GAP register error: %x", ret);
        return ret;
    }

    /* Set device name (visible on iPhone) */
    esp_ble_gap_set_device_name(device_name);
    esp_ble_gap_config_local_icon(ESP_BLE_APPEARANCE_GENERIC_WATCH);

    /* ── Security: IO capabilities for iOS numeric comparison ────── */
    /* iOS requires Display Yes/No capability for ANCS pairing.
     * Without this, security mode 4 defaults to level 0 → pairing
     * fails with "smp_calculate_link_key_from_long_term_key failed". */
    esp_ble_io_cap_t iocap = ESP_IO_CAP_IO;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));

    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;  /* LE Secure Connections + MITM + Bond */
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));

    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));

    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));

    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));

    ESP_LOGI(TAG, "BLE security: IO_CAP_IO, SC+MITM+Bond, key_size=16");

    /* Allocate data source reassembly buffer — try PSRAM first */
    data_buffer_ptr = heap_caps_malloc(DATA_BUFFER_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data_buffer_ptr) {
        data_buffer_ptr = heap_caps_malloc(DATA_BUFFER_SIZE, MALLOC_CAP_8BIT);
    }
    if (!data_buffer_ptr) {
        ESP_LOGE(TAG, "Failed to allocate data buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Data buffer: %d bytes @ %p", DATA_BUFFER_SIZE, data_buffer_ptr);
    /* ── End security config ─────────────────────────────────────── */

    /* Create timer for data source reassembly */
    const esp_timer_create_args_t timer_args = {
        .callback = &periodic_timer_callback,
        .name     = "ancs_ds",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &periodic_timer));

    /* Register GATT app — triggers ESP_GATTC_REG_EVT → advertising starts */
    ret = esp_ble_gattc_app_register(PROFILE_A_APP_ID);
    if (ret) {
        ESP_LOGE(TAG, "GATTC app register error: %x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "BLE ANCS initialized — device name: %s", device_name);
    return ESP_OK;
}

void app_ble_ancs_poll(void)
{
    /* Bluedroid is event-driven via callbacks; no polling needed.
     * This function is a placeholder for future use (e.g., connection
     * timeout handling). */
}

void app_ble_ancs_stop(void)
{
    if (!g_ancs_connected && !g_ancs_ready) return;

    ESP_LOGI(TAG, "Stopping BLE ANCS (thermal protection)...");

    /* Disconnect if connected */
    if (g_ancs_connected) {
        /* Try to disconnect from the peer device if we have an active GATT connection */
        esp_bd_addr_t addr;
        memcpy(addr, esp_bt_dev_get_address(), sizeof(esp_bd_addr_t));
        esp_ble_gap_disconnect(addr);
    }

    /* Stop advertising */
    esp_ble_gap_stop_advertising();

    /* Reset state */
    g_ancs_connected = false;
    g_ancs_ready     = false;
    g_paired         = false;
    g_cccd_ns_ok     = false;
    g_cccd_ds_ok     = false;

    ESP_LOGI(TAG, "BLE ANCS stopped");
}

void app_ble_ancs_restart(void)
{
    ESP_LOGI(TAG, "Restarting BLE ANCS (thermal recovery)...");

    /* Re-configure advertising data */
    esp_err_t ret = esp_ble_gap_config_adv_data(&adv_config);
    if (ret) {
        ESP_LOGW(TAG, "Config adv data failed: %x", ret);
        return;
    }

    ESP_LOGI(TAG, "BLE ANCS advertising restarted");
}

/* Cut the BLE radio for standby / light sleep.  Unlike app_ble_ancs_stop()
 * (which early-returns when never connected and leaves advertising running),
 * this ALWAYS stops advertising and drops any live link — with no advertising
 * and no connection there are zero scheduled RF events, so the BLE baseband
 * stays fully idle (radio off) while the ESP32 sleeps.  This removes the
 * ~1-2 mA of advertising / up-to-16 mA of connected-radio idle from the
 * sleep budget without the fragile controller disable/enable cycle. */
void app_ble_ancs_suspend(void)
{
    ESP_LOGI(TAG, "BLE → suspend (radio cut for standby)");

    esp_ble_gap_stop_advertising();

    /* Drop any live link so connection-event wakeups can't keep the radio on
     * either.  Use the stored peer address (remote_bda), not the local one. */
    if (g_ancs_connected) {
        esp_ble_gap_disconnect(gl_profile_tab[PROFILE_A_APP_ID].remote_bda);
    }

    g_ancs_connected = false;
    g_ancs_ready     = false;
    g_paired         = false;
    g_cccd_ns_ok     = false;
    g_cccd_ds_ok     = false;
}

void app_ble_ancs_resume(void)
{
    ESP_LOGI(TAG, "BLE → resume (restart advertising)");

    /* Re-configure adv data → the GAP ADV_DATA_SET_COMPLETE_EVT handler
     * restarts advertising.  No controller re-init needed. */
    esp_err_t ret = esp_ble_gap_config_adv_data(&adv_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE resume: config adv data failed: %x", ret);
    }
}
