/**
 * @file sim_mocks.c
 * @brief Definitions for the firmware globals and board functions that the
 *        UI layer expects to exist.
 *
 * The real implementations live in components/os and components/hw and are
 * tied to ESP-IDF services (NVS, PMIC, RTC, SD card).  Here they are either
 * backed by the host machine or by made-up values.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "app_config.h"
#include "app_events.h"
#include "app_image_loader.h"
#include "app_power.h"
#include "hw_display.h"
#include "hw_rtc.h"
#include "js_app_registry.h"
#include "sim_platform.h"
#include "watch_face.h"

#ifndef SIM_SPIFFS_ROOT
#define SIM_SPIFFS_ROOT "spiffs"
#endif

/* ── Global configuration ─────────────────────────────────────────── */
/* Keep the initialiser small: anything omitted is zero-initialised, and
 * every value here is a plausible factory default for the simulator. */
app_config_t g_config = {
    .wifi_enabled            = false,
    .ble_enabled             = false,
    .ancs_enabled            = false,
    .brightness              = 70,
    .font_scale              = 1,
    .always_on_display       = false,
    .wake_on_raise_enabled   = true,
    .menu_style              = 0,
    .dim_timeout_sec         = 5,
    .timezone_offset         = 8,
    .region_index            = 0,
    .ntp_url_index           = 0,
    .cpu_protection_enabled  = true,
    .power_chips_temp_threshold = 55,
    .cpu_temp_threshold      = 65,
    .honeycomb_distinct_icons = true,
    .honeycomb_layout_mode   = 1,
    .honeycomb_gap_adjust    = true,
    .vibrator_strength       = 255,
    .vibrator_duration_ms    = 100,
    .log_enabled             = true,
    .lang_code               = "en",
    .watch_face_id           = WATCH_FACE_MINIMAL,
    .flux_color_top          = 0xFF6B35,
    .flux_color_bot          = 0x1B1B1B,
    .hands_face_bg           = 0x000000,
    .hands_face_hour         = 0xFFFFFF,
    .hands_face_min          = 0xFFFFFF,
    .hands_face_sec          = 0xFF3B30,
};

/* ── Event-layer state ────────────────────────────────────────────── */
uint8_t g_brightness = 70;

bool g_pending_settings   = false;
bool g_pending_photos     = false;
bool g_pending_calculator = false;
bool g_pending_noise      = false;
bool g_pending_activity   = false;
bool g_pending_jsapps     = false;
bool g_pending_rearrange  = false;
int  g_pending_jsapp      = -1;

void app_events_request_vibe_click(void)
{
    /* No haptics on the host. */
}

/* ── Display backlight ────────────────────────────────────────────── */
esp_err_t hw_display_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    g_brightness = percent;
    return ESP_OK;
}

uint8_t hw_display_get_brightness(void)
{
    return g_brightness;
}

/* ── RTC: host wall clock ─────────────────────────────────────────── */
esp_err_t hw_rtc_get_time(hw_rtc_time_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    struct tm tm_now;
    if (!sim_use_real_time()) {
        /* Fixed demo clock: 10:08:30 gives Flux an interesting panel split
         * and keeps screenshots reproducible. */
        memset(&tm_now, 0, sizeof(tm_now));
        tm_now.tm_year = 126;   /* 2026 */
        tm_now.tm_mon  = 9;     /* October */
        tm_now.tm_mday = 19;
        tm_now.tm_wday = 1;     /* Monday */
        tm_now.tm_hour = 10;
        tm_now.tm_min  = 8;
        tm_now.tm_sec  = 30;
    } else {
        time_t now = time(NULL);
        localtime_r(&now, &tm_now);
    }

    out->sec   = (uint8_t)tm_now.tm_sec;
    out->min   = (uint8_t)tm_now.tm_min;
    out->hour  = (uint8_t)tm_now.tm_hour;
    out->day   = (uint8_t)tm_now.tm_mday;
    out->wday  = (uint8_t)tm_now.tm_wday;
    out->month = (uint8_t)(tm_now.tm_mon + 1);
    out->year  = (uint8_t)((tm_now.tm_year + 1900) % 100);
    return ESP_OK;
}

/* ── Power (AOD) ──────────────────────────────────────────────────── */
bool app_power_get_aod_active(void)
{
    return false;
}

/* ── Pre-decoded image loader ─────────────────────────────────────── */
/* Mirrors components/os/app_image_loader.c: the .bin files are byte-exact
 * copies of the original descriptor arrays, so the whole file is loaded
 * and data_size is the file size.  LVGL derives the stride from the
 * colour format, exactly as it does on the device. */
lv_image_dsc_t *app_img_load_raw(const char *path, uint32_t w, uint32_t h,
                                 lv_color_format_t cf)
{
    if (!path || w == 0 || h == 0) return NULL;

    /* "/spiffs/usr/..." → "<repo>/spiffs/usr/..." */
    const char *rel = path;
    if (strncmp(rel, "/spiffs", 7) == 0) rel += 7;
    while (*rel == '/') rel++;

    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", SIM_SPIFFS_ROOT, rel);

    FILE *f = fopen(full, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(f); return NULL; }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    lv_image_dsc_t *dsc = (lv_image_dsc_t *)calloc(1, sizeof(*dsc));
    if (!dsc) { free(data); return NULL; }

    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf    = (uint32_t)cf;
    dsc->header.w     = w;
    dsc->header.h     = h;
    dsc->data_size    = (uint32_t)size;
    dsc->data         = data;
    return dsc;
}

void app_img_free(lv_image_dsc_t *dsc)
{
    if (!dsc) return;
    free((void *)dsc->data);
    free(dsc);
}

/* ── JS app registry (SD card apps are not simulated) ─────────────── */
int js_app_registry_scan(void) { return 0; }
int js_app_registry_count(void) { return 0; }

const js_app_entry_t *js_app_registry_get(int index)
{
    (void)index;
    return NULL;
}

const char *js_app_registry_entry_path(int index)
{
    (void)index;
    return NULL;
}

/* ── Parallax face ────────────────────────────────────────────────── */
/* The real face needs the AI-generated depth layers, which are produced
 * offline into spiffs/ and are not part of the source tree.  Register a
 * placeholder so the face list and picker stay consistent. */
static lv_obj_t *parallax_placeholder_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Parallax");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "depth layers not built\nrun tools/parallax_face/");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8899AA), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 24);
    return scr;
}

static void parallax_placeholder_destroy(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (scr) lv_obj_delete(scr);
}

const watch_face_t watch_face_parallax = {
    .name           = "Parallax",
    .id             = WATCH_FACE_PARALLAX,
    .create         = parallax_placeholder_create,
    .destroy        = parallax_placeholder_destroy,
    .update_time    = NULL,
    .update_battery = NULL,
    .aod_pixel_shift = NULL,
};
