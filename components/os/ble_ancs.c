/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Adapted from ESP-IDF ble_ancs example.
 * Added callback mechanism so the application layer receives parsed
 * notification data instead of only logging it.
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "ble_ancs.h"

#define BLE_ANCS_TAG  "BLE_ANCS"

/* ── Callback storage ─────────────────────────────────────────────── */

static ancs_event_callback_t        g_ancs_event_cb        = NULL;
static ancs_notification_callback_t g_ancs_notification_cb = NULL;

void ancs_register_event_callback(ancs_event_callback_t cb)
{
    g_ancs_event_cb = cb;
}

void ancs_register_notification_callback(ancs_notification_callback_t cb)
{
    g_ancs_notification_cb = cb;
}

/* ── Helpers ──────────────────────────────────────────────────────── */

char *EventID_to_String(uint8_t EventID)
{
    switch (EventID) {
        case EventIDNotificationAdded:    return "New message";
        case EventIDNotificationModified: return "Modified message";
        case EventIDNotificationRemoved:  return "Removed message";
        default:                          return "unknown EventID";
    }
}

char *CategoryID_to_String(uint8_t CategoryID)
{
    switch (CategoryID) {
        case CategoryIDOther:              return "Other";
        case CategoryIDIncomingCall:       return "IncomingCall";
        case CategoryIDMissedCall:         return "MissedCall";
        case CategoryIDVoicemail:          return "Voicemail";
        case CategoryIDSocial:             return "Social";
        case CategoryIDSchedule:           return "Schedule";
        case CategoryIDEmail:              return "Email";
        case CategoryIDNews:               return "News";
        case CategoryIDHealthAndFitness:   return "HealthAndFitness";
        case CategoryIDBusinessAndFinance: return "BusinessAndFinance";
        case CategoryIDLocation:           return "Location";
        case CategoryIDEntertainment:      return "Entertainment";
        default:                           return "Unknown CategoryID";
    }
}

char *Errcode_to_String(uint16_t status)
{
    switch (status) {
        case Unknown_command:   return "Unknown_command";
        case Invalid_command:   return "Invalid_command";
        case Invalid_parameter: return "Invalid_parameter";
        case Action_failed:     return "Action_failed";
        default:                return "unknown_failed";
    }
}

/* ── Helper: safely copy UTF-8 string from buffer ─────────────────── */

static void safe_copy_str(char *dst, size_t dst_size,
                          const uint8_t *src, uint16_t src_len)
{
    if (!dst || !src) return;
    size_t copy_len = (src_len < dst_size - 1) ? src_len : dst_size - 1;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

/* ── Notification Source (Event) ──────────────────────────────────── */

/*
 * Packet format:
 * | EventID(1) | EventFlags(1) | CategoryID(1) | CategoryCount(1) | NotificationUID(4) |
 */
void esp_receive_apple_notification_source(uint8_t *message, uint16_t message_len)
{
    if (!message || message_len < 5) {
        return;
    }

    uint8_t  EventID        = message[0];
    uint8_t  EventFlags     = message[1];
    uint8_t  CategoryID     = message[2];
    uint8_t  CategoryCount  = message[3];
    uint32_t NotificationUID = (message[4]) | (message[5] << 8) |
                               (message[6] << 16) | (message[7] << 24);

    ESP_LOGI(BLE_ANCS_TAG, "EventID:%s EventFlags:0x%x CategoryID:%s "
             "CategoryCount:%d NotificationUID:%" PRIu32,
             EventID_to_String(EventID), EventFlags,
             CategoryID_to_String(CategoryID), CategoryCount, NotificationUID);

    /* Fire event callback — use PSRAM struct to avoid ~840 bytes on
     * BTC stack AND to keep internal DRAM free for BLE stack buffers. */
    if (g_ancs_event_cb) {
        static EXT_RAM_BSS_ATTR ancs_notification_t notif;
        memset(&notif, 0, sizeof(notif));
        notif.uid            = NotificationUID;
        notif.event_id       = EventID;
        notif.event_flags    = EventFlags;
        notif.category_id    = CategoryID;
        notif.category_count = CategoryCount;
        g_ancs_event_cb(&notif);
    }
}

/* ── Data Source (Notification Attributes) ────────────────────────── */

void esp_receive_apple_data_source(uint8_t *message, uint16_t message_len)
{
    if (!message || message_len == 0) {
        return;
    }

    uint8_t Command_id = message[0];
    switch (Command_id) {
    case CommandIDGetNotificationAttributes: {
        if (message_len < 5) {
            ESP_LOGE(BLE_ANCS_TAG, "Message too short for NotificationAttributes");
            break;
        }

        uint32_t NotificationUID = (message[1]) | (message[2] << 8) |
                                   (message[3] << 16) | (message[4] << 24);
        uint32_t remian_attr_len = message_len - 5;
        uint8_t *attrs = &message[5];

        ESP_LOGI(BLE_ANCS_TAG, "Notification attributes response, UID %" PRIu32,
                 NotificationUID);

        /* Build a notification struct for the callback — PSRAM to avoid
         * ~840 bytes on BTC task stack AND internal DRAM pressure. */
        static EXT_RAM_BSS_ATTR ancs_notification_t notif;
        memset(&notif, 0, sizeof(notif));
        notif.uid = NotificationUID;

        while (remian_attr_len > 0) {
            if (remian_attr_len < 3) {
                ESP_LOGE(BLE_ANCS_TAG, "Incomplete attribute header");
                break;
            }
            uint8_t  AttributeID = attrs[0];
            uint16_t len = attrs[1] | (attrs[2] << 8);
            if (len > (remian_attr_len - 3)) {
                ESP_LOGE(BLE_ANCS_TAG, "data error");
                break;
            }

            switch (AttributeID) {
            case NotificationAttributeIDAppIdentifier:
                ESP_LOG_BUFFER_CHAR("Identifier", &attrs[3], len);
                safe_copy_str(notif.app_id, sizeof(notif.app_id),
                              &attrs[3], len);
                notif.has_app_id = true;
                break;
            case NotificationAttributeIDTitle:
                ESP_LOG_BUFFER_CHAR("Title", &attrs[3], len);
                safe_copy_str(notif.title, sizeof(notif.title),
                              &attrs[3], len);
                notif.has_title = true;
                break;
            case NotificationAttributeIDSubtitle:
                ESP_LOG_BUFFER_CHAR("Subtitle", &attrs[3], len);
                safe_copy_str(notif.subtitle, sizeof(notif.subtitle),
                              &attrs[3], len);
                notif.has_subtitle = true;
                break;
            case NotificationAttributeIDMessage:
                ESP_LOG_BUFFER_CHAR("Message", &attrs[3], len);
                safe_copy_str(notif.message, sizeof(notif.message),
                              &attrs[3], len);
                notif.has_message = true;
                break;
            case NotificationAttributeIDMessageSize:
                ESP_LOG_BUFFER_CHAR("MessageSize", &attrs[3], len);
                break;
            case NotificationAttributeIDDate:
                ESP_LOG_BUFFER_CHAR("Date", &attrs[3], len);
                break;
            case NotificationAttributeIDPositiveActionLabel:
                ESP_LOG_BUFFER_HEX("PActionLabel", &attrs[3], len);
                break;
            case NotificationAttributeIDNegativeActionLabel:
                ESP_LOG_BUFFER_HEX("NActionLabel", &attrs[3], len);
                break;
            default:
                ESP_LOG_BUFFER_HEX("unknownAttributeID", &attrs[3], len);
                break;
            }

            attrs += (1 + 2 + len);
            remian_attr_len -= (1 + 2 + len);
        }

        /* Fire notification callback with all parsed attributes */
        if (g_ancs_notification_cb) {
            g_ancs_notification_cb(&notif);
        }
        break;
    }
    case CommandIDGetAppAttributes:
        ESP_LOGI(BLE_ANCS_TAG, "App Attributes response");
        break;
    case CommandIDPerformNotificationAction:
        ESP_LOGI(BLE_ANCS_TAG, "Perform Notification Action");
        break;
    default:
        ESP_LOGI(BLE_ANCS_TAG, "unknown Command ID");
        break;
    }
}
