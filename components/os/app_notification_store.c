/**
 * @file app_notification_store.c
 * @brief Notification data store — see app_notification_store.h
 */

#include "app_notification_store.h"
#include "hw_rtc.h"
#include "app_config.h"
#include "png_images_rgb565.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

static const char *TAG = "NOTIF_STORE";

/* ── Storage ───────────────────────────────────────────────────────── */

static app_notification_t s_items[APP_NOTIF_MAX_COUNT];
static int      s_count = 0;
static uint32_t s_local_uid = 0x80000000u;  /* uid space for app-sent messages */

/* ── Time helper ───────────────────────────────────────────────────── */

static time_t now_unix(void)
{
    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) == ESP_OK) {
        return hw_rtc_to_unix(&rt);
    }
    return time(NULL);
}

/* ── Store API ─────────────────────────────────────────────────────── */

int app_notification_store_add_ancs(const ancs_notification_t *n)
{
    if (!n) return -1;

    /* If the same UID is already stored, just refresh its content
     * (ANCS re-sends attributes after a "modified" event). */
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].uid == n->uid) {
            if (n->has_title && n->title[0]) {
                snprintf(s_items[i].title, sizeof(s_items[i].title), "%s", n->title);
            }
            if (n->has_message && n->message[0]) {
                snprintf(s_items[i].message, sizeof(s_items[i].message), "%s", n->message);
            }
            ESP_LOGI(TAG, "updated uid=%lu", (unsigned long)n->uid);
            return i;
        }
    }

    /* Build the new entry, then insert at the TOP (newest first). */
    app_notification_t it;
    memset(&it, 0, sizeof(it));
    it.uid         = n->uid;
    it.source      = APP_NOTIF_SOURCE_PHONE;
    it.category_id = n->category_id;
    snprintf(it.app_id, sizeof(it.app_id), "%s",
             (n->has_app_id && n->app_id[0]) ? n->app_id : "");
    snprintf(it.title, sizeof(it.title), "%s",
             (n->has_title && n->title[0]) ? n->title : "");
    snprintf(it.message, sizeof(it.message), "%s",
             (n->has_message && n->message[0]) ? n->message : "");
    it.timestamp = now_unix();

    /* Overflow → drop the oldest notification (kept at the tail). */
    if (s_count >= APP_NOTIF_MAX_COUNT) {
        memmove(&s_items[1], &s_items[0],
                sizeof(app_notification_t) * (size_t)(APP_NOTIF_MAX_COUNT - 1));
    } else {
        memmove(&s_items[1], &s_items[0],
                sizeof(app_notification_t) * (size_t)s_count);
        s_count++;
    }
    s_items[0] = it;

    ESP_LOGI(TAG, "added uid=%lu app=%s | %s", (unsigned long)n->uid,
             it.app_id, it.title);
    return 0;
}

int app_notification_store_add_app(const char *app_id, const char *title, const char *msg)
{
    /* Build the new entry, then insert at the TOP (newest first). */
    app_notification_t it;
    memset(&it, 0, sizeof(it));
    it.uid         = s_local_uid++;
    it.source      = APP_NOTIF_SOURCE_APP;
    it.category_id = 0;
    snprintf(it.app_id, sizeof(it.app_id), "%s", app_id ? app_id : "");
    snprintf(it.title, sizeof(it.title), "%s", title ? title : "");
    snprintf(it.message, sizeof(it.message), "%s", msg ? msg : "");
    it.timestamp = now_unix();

    if (s_count >= APP_NOTIF_MAX_COUNT) {
        memmove(&s_items[1], &s_items[0],
                sizeof(app_notification_t) * (size_t)(APP_NOTIF_MAX_COUNT - 1));
    } else {
        memmove(&s_items[1], &s_items[0],
                sizeof(app_notification_t) * (size_t)s_count);
        s_count++;
    }
    s_items[0] = it;

    ESP_LOGI(TAG, "added(app) %s | %s", it.app_id, it.title);
    return 0;
}

void app_notification_store_remove(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    memmove(&s_items[idx], &s_items[idx + 1],
            sizeof(app_notification_t) * (size_t)(s_count - idx - 1));
    s_count--;
}

void app_notification_store_clear(void)
{
    memset(s_items, 0, sizeof(s_items));
    s_count = 0;
}

int app_notification_store_count(void)
{
    return s_count;
}

const app_notification_t *app_notification_store_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_items[idx];
}

/* ── Relative time formatting ──────────────────────────────────────── */

void app_notification_store_format_time(time_t ts, char *buf, size_t len)
{
    if (!buf || len == 0) return;
    buf[0] = '\0';

    time_t now = now_unix();
    int64_t diff = (int64_t)now - (int64_t)ts;
    if (diff < 0) diff = 0;

    if (diff < 60) {
        snprintf(buf, len, "now");
    } else if (diff < 3600) {
        snprintf(buf, len, "%lldm ago", (long long)(diff / 60));
    } else if (diff < 86400) {
        long long h = diff / 3600;
        long long m = (diff % 3600) / 60;
        if (m == 0) {
            snprintf(buf, len, "%lldh ago", h);
        } else {
            snprintf(buf, len, "%lldh %lldm ago", h, m);
        }
    } else {
        /* Older than 24h — show a local date (respects timezone config). */
        time_t local_ts = ts + (time_t)(g_config.timezone_offset * 3600);
        hw_rtc_time_t lt;
        hw_rtc_from_unix(local_ts, &lt);
        snprintf(buf, len, "%02d/%02d", lt.month, lt.day);
    }
}

/* ── Icon resolution ───────────────────────────────────────────────── */

/* Case-insensitive substring search (strcasestr may not be linked). */
static const char *ci_substr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;

    while (*haystack) {
        size_t i = 0;
        while (i < nlen && haystack[i] != '\0') {
            char a = haystack[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nlen) return haystack;
        haystack++;
    }
    return NULL;
}

#define ICON_ENTRY(key, icon) { key, &(icon) }

static const lv_image_dsc_t *match_icon(const char *app_id)
{
    if (!app_id || app_id[0] == '\0') return NULL;

    static const struct {
        const char *key;
        const lv_image_dsc_t *icon;
    } s_icon_map[] = {
        /* Messages / calls */
        ICON_ENTRY("MobileSMS",        imessage_96),
        ICON_ENTRY("message",          imessage_96),
        ICON_ENTRY("imessage",         imessage_96),
        ICON_ENTRY("InCallService",    phone_96),
        ICON_ENTRY("mobilephone",      phone_96),
        ICON_ENTRY("Facetime",         phone_96),
        ICON_ENTRY("phone",            phone_96),
        /* Mail */
        ICON_ENTRY("mobilemail",       mail_96),
        ICON_ENTRY("mail",             mail_96),
        /* Photos / camera */
        ICON_ENTRY("MobileSlideShow",  photos_96),
        ICON_ENTRY("photos",           photos_96),
        ICON_ENTRY("camera",           camera_96),
        /* Weather / maps */
        ICON_ENTRY("weather",          weather_96),
        ICON_ENTRY("Maps",             maps_96),
        /* Calendar / reminders / timer */
        ICON_ENTRY("mobilecal",        calendar_96),
        ICON_ENTRY("calendar",         calendar_96),
        ICON_ENTRY("reminders",        reminders_96),
        ICON_ENTRY("MobileTimer",      timer_96),
        ICON_ENTRY("timer",            timer_96),
        /* System */
        ICON_ENTRY("Preferences",      settings_96),
        ICON_ENTRY("settings",         settings_96),
        ICON_ENTRY("MobileSafari",     world_clock_96),
        ICON_ENTRY("safari",           world_clock_96),
        ICON_ENTRY("MobileSafari",     world_clock_96),
        /* Media */
        ICON_ENTRY("Music",            Music_96),
        ICON_ENTRY("Podcasts",         Padcasts_96),
        ICON_ENTRY("itunes",           itunes_96),
        ICON_ENTRY("Music",            Music_96),
        ICON_ENTRY("Now_Playing",      Now_Playing_96),
        ICON_ENTRY("Audiobooks",       Audiobooks_96),
        ICON_ENTRY("VoiceMemos",       Voice_Memos_96),
        ICON_ENTRY("Remote",           remote_96),
        /* News / stocks / wallet */
        ICON_ENTRY("news",             News_96),
        ICON_ENTRY("stocks",           stocks_96),
        ICON_ENTRY("Wallet",           wallet_96),
        ICON_ENTRY("AppStore",         app_store_96),
        ICON_ENTRY("appstore",         app_store_96),
        /* Health / fitness */
        ICON_ENTRY("fitness",          workout_96),
        ICON_ENTRY("Activity",         Activity_96),
        ICON_ENTRY("workout",          workout_96),
        ICON_ENTRY("Health",           Heart_Rate_96),
        ICON_ENTRY("Heart",            Heart_Rate_96),
        ICON_ENTRY("Vitals",           Vitals_96),
        ICON_ENTRY("ECG",              ECG_96),
        ICON_ENTRY("Sleep",            Sleep_96),
        ICON_ENTRY("Mindfulness",      Mindfulness_96),
        ICON_ENTRY("Cycle",            Cycle_Tracking_96),
        ICON_ENTRY("Depth",            Depth_96),
        ICON_ENTRY("Medicine",         Medicine_96),
        ICON_ENTRY("Noise",            Noise_96),
        ICON_ENTRY("Siren",            Siren_96),
        ICON_ENTRY("Tips",             Tips_96),
        /* Apps / misc */
        ICON_ENTRY("Contacts",         Contacts_96),
        ICON_ENTRY("FindMy",           Find_Devices_96),
        ICON_ENTRY("Home",             home_96),
        ICON_ENTRY("calculator",       calculator_96),
        ICON_ENTRY("alarm",            alarm_clock_96),
        ICON_ENTRY("clock",            clock_96),
        ICON_ENTRY("watch",            clock_96),
    };

    for (size_t i = 0; i < sizeof(s_icon_map) / sizeof(s_icon_map[0]); i++) {
        if (ci_substr(app_id, s_icon_map[i].key) != NULL) {
            return s_icon_map[i].icon;
        }
    }
    return NULL;
}

const lv_image_dsc_t *app_notification_store_resolve_icon(const char *app_id)
{
    return match_icon(app_id);
}

const lv_image_dsc_t *app_notification_store_default_icon(void)
{
    return &app_store_96;
}
