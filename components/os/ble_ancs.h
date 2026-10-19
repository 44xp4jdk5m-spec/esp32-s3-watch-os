/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Adapted from ESP-IDF ble_ancs example for watch project.
 * Added callback mechanism for application-layer notification handling.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── ANCS Protocol Enums ──────────────────────────────────────────── */

// EventID values
typedef enum {
    EventIDNotificationAdded    = 0,
    EventIDNotificationModified = 1,
    EventIDNotificationRemoved  = 2,
} esp_EventID;

// EventFlags
typedef enum {
    EventFlagSilent         = (1 << 0),
    EventFlagImportant      = (1 << 1),
    EventFlagPreExisting    = (1 << 2),
    EventFlagPositiveAction = (1 << 3),
    EventFlagNegativeAction = (1 << 4),
} esp_EventFlags;

// CategoryID values
typedef enum {
    CategoryIDOther              = 0,
    CategoryIDIncomingCall       = 1,
    CategoryIDMissedCall         = 2,
    CategoryIDVoicemail          = 3,
    CategoryIDSocial             = 4,
    CategoryIDSchedule           = 5,
    CategoryIDEmail              = 6,
    CategoryIDNews               = 7,
    CategoryIDHealthAndFitness   = 8,
    CategoryIDBusinessAndFinance = 9,
    CategoryIDLocation           = 10,
    CategoryIDEntertainment      = 11,
} esp_CategoryID;

// CommandID values
typedef enum {
    CommandIDGetNotificationAttributes = 0,
    CommandIDGetAppAttributes          = 1,
    CommandIDPerformNotificationAction = 2,
} esp_CommandID;

// NotificationAttributeID
typedef enum {
    NotificationAttributeIDAppIdentifier       = 0,
    NotificationAttributeIDTitle               = 1,
    NotificationAttributeIDSubtitle            = 2,
    NotificationAttributeIDMessage             = 3,
    NotificationAttributeIDMessageSize         = 4,
    NotificationAttributeIDDate                = 5,
    NotificationAttributeIDPositiveActionLabel = 6,
    NotificationAttributeIDNegativeActionLabel = 7,
} esp_NotificationAttributeID;

// ActionID values
typedef enum {
    ActionIDPositive = 0,
    ActionIDNegative = 1,
} esp_ActionID;

// AppAttributeID Values
typedef enum {
    AppAttributeIDDisplayName = 0,
} esp_AppAttributeID;

typedef struct {
    uint8_t  noti_attribute_id;
    uint16_t attribute_len;
} esp_noti_attr_list_t;

typedef enum {
    Unknown_command   = 0xA0,
    Invalid_command   = 0xA1,
    Invalid_parameter = 0xA2,
    Action_failed     = 0xA3,
} esp_error_code;

typedef enum {
    attr_appidentifier_index   = 0,
    attr_title_index,
    attr_subtitle_index,
    attr_message_index,
    attr_messagesize_index,
    attr_date_index,
    attr_positiveactionlabel_index,
    attr_negativeactionlabel_index,
} esp_attr_index;

#define ESP_NOTIFICATIONUID_LEN   4

/* ── Notification data structure ──────────────────────────────────── */

/** Maximum length for notification text fields (UTF-8) */
#define ANCS_MAX_APP_ID_LEN    64
#define ANCS_MAX_TITLE_LEN     128
#define ANCS_MAX_SUBTITLE_LEN  128
#define ANCS_MAX_MESSAGE_LEN   512

typedef struct {
    uint32_t uid;                                    /* Notification UID */
    uint8_t  event_id;                               /* Added / Modified / Removed */
    uint8_t  event_flags;                            /* Silent, Important, etc. */
    uint8_t  category_id;                            /* Call, Email, Social, etc. */
    uint8_t  category_count;                         /* Active count in category */
    char     app_id[ANCS_MAX_APP_ID_LEN];            /* App identifier (bundle ID) */
    char     title[ANCS_MAX_TITLE_LEN];              /* Notification title */
    char     subtitle[ANCS_MAX_SUBTITLE_LEN];         /* Notification subtitle */
    char     message[ANCS_MAX_MESSAGE_LEN];          /* Notification message body */
    bool     has_app_id;
    bool     has_title;
    bool     has_subtitle;
    bool     has_message;
} ancs_notification_t;

/* ── Callback types ───────────────────────────────────────────────── */

/**
 * @brief Called when a notification event is received (added/modified/removed).
 *        For "added" events, this fires BEFORE the detailed attributes arrive.
 * @param notif  Parsed notification with event info (title/message may be empty yet)
 */
typedef void (*ancs_event_callback_t)(const ancs_notification_t *notif);

/**
 * @brief Called when notification attributes (title, message, etc.) are received.
 *        This fires AFTER the NC requests detailed attributes from the NP.
 * @param notif  Fully populated notification data
 */
typedef void (*ancs_notification_callback_t)(const ancs_notification_t *notif);

/* ── API ───────────────────────────────────────────────────────────── */

char *EventID_to_String(uint8_t EventID);
char *CategoryID_to_String(uint8_t CategoryID);
char *Errcode_to_String(uint16_t status);

/**
 * @brief Register callback for notification source events (added/modified/removed).
 */
void ancs_register_event_callback(ancs_event_callback_t cb);

/**
 * @brief Register callback for full notification data (title, message, etc.).
 */
void ancs_register_notification_callback(ancs_notification_callback_t cb);

/**
 * @brief Parse ANCS Notification Source characteristic data.
 *        Called internally by the BLE GATT event handler.
 */
void esp_receive_apple_notification_source(uint8_t *message, uint16_t message_len);

/**
 * @brief Parse ANCS Data Source characteristic data.
 *        Called internally by the BLE GATT event handler.
 */
void esp_receive_apple_data_source(uint8_t *message, uint16_t message_len);

#ifdef __cplusplus
}
#endif
