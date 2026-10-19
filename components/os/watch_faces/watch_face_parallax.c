/**
 * @file watch_face_parallax.c
 * @brief "Parallax" watch face — Facebook-3D-style depth photo.
 *
 * The same trick Facebook 3D photos use (2D image + depth), pre-baked so the
 * ESP32-S3 never has to warp anything:
 *
 *   - tools/parallax_face/build_parallax_face.py runs a REAL AI depth model
 *     (Depth Anything V2, ONNX on the PC) so the layers follow OBJECT EDGES, then
 *     slices the photo into N depth layers.  Layers are cropped to their alpha
 *     bounding box and pre-decoded into raw LVGL RGB565A8 bytes on the PC
 *     (spiffs/usr/share/watchfaces/parallax/layerN.bin + meta.txt) — so the watch does NO on-device
 *     decoding, just fopen + read into PSRAM.
 *   - At runtime this face stacks the layers as lv_image objects far→near and
 *     slides each one by a per-depth offset driven by the QMI8658 gravity vector.
 *     Tilting the watch moves the foreground more than the background → smooth
 *     2.5D parallax.
 *
 * Memory: full-screen (410x502) RGB565A8 (3 B/px) + alpha-bbox crop ≈ 2.2 MB
 * PSRAM for the default 5-layer portrait demo.  Tune layer count / canvas /
 * shift here — must match spiffs/usr/share/watchfaces/parallax/meta.txt.
 *
 * IMU note: reuses the air_pointer roll/pitch convention
 *   roll  = atan2(ay, sqrt(ax²+az²)),  pitch = atan2(ax, sqrt(ay²+az²)).
 * The background (farthest) layer moves least, the foreground most.
 */

#include "watch_face.h"

#include "hw_display.h"
#include "hw_imu.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fonts.h"
#include "ui.h"          /* minimal-face fonts (ui_font_NotoSansCondensedBold80 etc.) */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "WF_PFX";

/* ── Geometry (must match spiffs/usr/share/watchfaces/parallax/meta.txt
 *    + the Python builder) ── */
#define PF_CANVAS_X     0     /* full-screen photo canvas */
#define PF_CANVAS_Y     0
#define PF_CANVAS_W     410
#define PF_CANVAS_H     502
#define PF_MARGIN       26    /* extra pixels around the canvas for shifts */
#define PF_MAX_SHIFT    24    /* max parallax offset in px (must be <= margin) */
#define PF_MAX_LAYERS   8

#define PF_DIR          "/spiffs/usr/share/watchfaces/parallax"
#define PF_META         "/spiffs/usr/share/watchfaces/parallax/meta.txt"

/* ── Parallax feel ─────────────────────────────────────────────────── */
#define PF_ANGLE_MAX_DEG  25.0f  /* tilt angle (deg) that gives full max shift */
#define PF_FAR_FACTOR    0.40f  /* farthest layer: this × max shift.  The background
                                   moves WITH the subject so it doesn't feel detached */
#define PF_NEAR_FACTOR   0.85f  /* nearest layer: this × max shift (kept < 1.0 so the
                                   foreground doesn't swing too hard past the others) */
#define PF_X_SIGN        -1.0f   /* sign of the horizontal parallax */
#define PF_Y_SIGN         1.0f   /* sign of the vertical parallax */
#define PF_SMOOTH         0.25f  /* tilt low-pass (0..1, higher = snappier) */
#define PF_SAMPLE_MS      33     /* IMU sample period (~30 Hz) */
#define PF_TASK_STACK     4096
#define PF_TASK_PRIO      5
#define PF_STOP_WAIT_MS   500

#define PF_DEG2RAD        0.01745329252f

/* ── One loaded depth layer ────────────────────────────────────────── */
typedef struct {
    uint8_t        *pix;     /* RGB565 color + A8 alpha planes, heap_caps SPIRAM */
    uint32_t        stride;  /* color row stride (w*2); alpha map at pix+stride*h */
    lv_image_dsc_t  dsc;
    int             off_x;   /* crop origin within the full-size layer */
    int             off_y;
    int             w, h;
} pf_layer_t;

static lv_obj_t        *s_screen   = NULL;
static lv_obj_t        *s_imgs[PF_MAX_LAYERS];
static pf_layer_t       s_layers[PF_MAX_LAYERS];
static int              s_n = 0;            /* layers actually loaded */
static lv_obj_t        *s_time_lbl = NULL;
static lv_obj_t        *s_date_lbl = NULL;
static lv_obj_t        *s_batt_lbl = NULL;

static TaskHandle_t      s_task    = NULL;
static SemaphoreHandle_t s_stopped = NULL;
static volatile bool     s_stop    = false;

/* Smoothed tilt factors in [-1,1] (0 = watch flat).  Written by the IMU task,
 * read by pf_reposition() (also called from aod_pixel_shift). */
static volatile float    s_fx = 0.0f, s_fy = 0.0f;
static int               s_aod_dx = 0, s_aod_dy = 0;

static void pf_reposition(void);

/* ── Layer loading ────────────────────────────────────────────────────
 * The builder pre-decodes each layer to raw LVGL RGB565A8 bytes (RGB565 color
 * plane, stride w*2, immediately followed by an A8 alpha plane, stride w → 3
 * B/px).  Loading is just fopen + one read into a PSRAM buffer — no on-device
 * decode, so it is fast and needs little transient memory.
 *   layer0 is drawn first (farthest, alpha = its depth band). */
static uint8_t *pf_load_raw(const char *path, uint32_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        ESP_LOGE(TAG, "%s empty", path);
        fclose(f);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM alloc %ld B for %s failed", sz, path);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        heap_caps_free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (uint32_t)sz;
    return buf;
}

/* Read spiffs/usr/share/watchfaces/parallax/meta.txt:
 *   line 1: layers fullW fullH canvasW canvasH margin maxShift
 *   one line per layer: off_x off_y w h   (crop within the full-size layer) */
static bool pf_load_meta(void)
{
    FILE *f = fopen(PF_META, "r");
    if (!f) {
        ESP_LOGE(TAG, "no %s — run tools/parallax_face/build_parallax_face.py", PF_META);
        return false;
    }
    int n = 0, fw = 0, fh = 0, cw = 0, ch = 0, margin = 0, mshift = 0;
    /* Consume the WHOLE header line — otherwise its trailing numbers leak into
     * the first layer's crop record and every size check fails. */
    if (fscanf(f, "%d %d %d %d %d %d %d", &n, &fw, &fh, &cw, &ch, &margin, &mshift) != 7
        || n <= 0 || n > PF_MAX_LAYERS) {
        ESP_LOGE(TAG, "bad meta.txt header");
        fclose(f);
        return false;
    }
    s_n = n;
    for (int i = 0; i < n; i++) {
        if (fscanf(f, "%d %d %d %d", &s_layers[i].off_x, &s_layers[i].off_y,
                   &s_layers[i].w, &s_layers[i].h) != 4) {
            s_layers[i].off_x = 0;
            s_layers[i].off_y = 0;
            s_layers[i].w = 0;
            s_layers[i].h = 0;
        }
    }
    fclose(f);
    return true;
}

/* ── IMU-driven parallax ──────────────────────────────────────────── */
static void pf_reposition(void)
{
    if (!s_screen || !lv_obj_is_valid(s_screen)) return;
    if (lv_screen_active() != s_screen) return;   /* off-screen (picker preview) */

    int cx = PF_CANVAS_X - PF_MARGIN;
    int cy = PF_CANVAS_Y - PF_MARGIN;
    for (int i = 0; i < s_n; i++) {
        if (!s_layers[i].pix || !s_imgs[i] || !lv_obj_is_valid(s_imgs[i])) continue;
        float depth = (s_n > 1) ? (float)i / (s_n - 1) : 0.0f;   /* 0 far … 1 near */
        float k     = PF_FAR_FACTOR + (PF_NEAR_FACTOR - PF_FAR_FACTOR) * depth;
        int   ox    = (int)lroundf(PF_X_SIGN * s_fx * PF_MAX_SHIFT * k);
        int   oy    = (int)lroundf(PF_Y_SIGN * s_fy * PF_MAX_SHIFT * k);
        lv_obj_set_pos(s_imgs[i],
                       cx + s_layers[i].off_x + ox + s_aod_dx,
                       cy + s_layers[i].off_y + oy + s_aod_dy);
    }
    /* shift the HUD labels with AOD pixel-shift (like the layers) */
    if (s_time_lbl && lv_obj_is_valid(s_time_lbl)) lv_obj_set_pos(s_time_lbl, s_aod_dx, -26 + s_aod_dy);
    if (s_date_lbl && lv_obj_is_valid(s_date_lbl)) lv_obj_set_pos(s_date_lbl, s_aod_dx, 62 + s_aod_dy);
    if (s_batt_lbl && lv_obj_is_valid(s_batt_lbl)) lv_obj_set_pos(s_batt_lbl, -14 + s_aod_dx, 10 + s_aod_dy);
}

static void pf_task(void *arg)
{
    (void)arg;
    hw_imu_data_t d;
    bool imu_ok = true;
    while (!s_stop) {
        if (imu_ok && hw_imu_read(&d) == ESP_OK) {
            float roll  = atan2f(d.accel_y, sqrtf(d.accel_x * d.accel_x + d.accel_z * d.accel_z));
            float pitch = atan2f(d.accel_x, sqrtf(d.accel_y * d.accel_y + d.accel_z * d.accel_z));
            float fx = roll  / (PF_ANGLE_MAX_DEG * PF_DEG2RAD);
            float fy = pitch / (PF_ANGLE_MAX_DEG * PF_DEG2RAD);
            if (fx > 1.0f) fx = 1.0f; else if (fx < -1.0f) fx = -1.0f;
            if (fy > 1.0f) fy = 1.0f; else if (fy < -1.0f) fy = -1.0f;
            s_fx += (fx - s_fx) * PF_SMOOTH;
            s_fy += (fy - s_fy) * PF_SMOOTH;

            if (hw_display_lock(30)) {
                pf_reposition();
                hw_display_unlock();
            }
        } else if (imu_ok) {
            imu_ok = false;   /* IMU absent — parallax just stays static */
        }
        vTaskDelay(pdMS_TO_TICKS(PF_SAMPLE_MS));
    }
    if (s_stopped) xSemaphoreGive(s_stopped);
    vTaskDelete(NULL);
}

/* ── watch_face_t implementation ──────────────────────────────────── */
static lv_obj_t *pf_create(void)
{
    if (s_screen) return s_screen;

    memset(s_layers, 0, sizeof(s_layers));
    memset(s_imgs,   0, sizeof(s_imgs));
    s_n = 0;

    /* 1) load the layers (fast: one fopen+read each, outside the LVGL lock) */
    pf_load_meta();
    size_t total_bytes = 0;
    int    loaded = 0;
    for (int i = 0; i < s_n; i++) {
        if (s_layers[i].w <= 0 || s_layers[i].h <= 0) {
            ESP_LOGE(TAG, "layer%d bad size from meta", i);
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), PF_DIR "/layer%d.bin", i);
        uint32_t sz = 0;
        s_layers[i].pix = pf_load_raw(path, &sz);
        if (!s_layers[i].pix) {
            ESP_LOGE(TAG, "layer%d load failed", i);
            continue;
        }
        /* layer0 = opaque full-photo background (RGB565, 2 B/px); the rest are
         * RGB565A8 depth bands (3 B/px). */
        uint32_t expected = (i == 0)
                                ? (uint32_t)s_layers[i].w * (uint32_t)s_layers[i].h * 2
                                : (uint32_t)s_layers[i].w * (uint32_t)s_layers[i].h * 3;
        if (sz != expected) {
            ESP_LOGE(TAG, "layer%d size mismatch (%lu != %lu)", i,
                     (unsigned long)sz, (unsigned long)expected);
            heap_caps_free(s_layers[i].pix);
            s_layers[i].pix = NULL;
            continue;
        }
        loaded++;
        s_layers[i].stride = (uint32_t)s_layers[i].w * 2;   /* color stride */
        lv_image_dsc_t *d = &s_layers[i].dsc;
        memset(d, 0, sizeof(*d));
        d->header.cf       = (i == 0) ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
        d->header.w        = s_layers[i].w;
        d->header.h        = s_layers[i].h;
        d->header.stride   = s_layers[i].stride;
        d->data_size       = sz;
        d->data            = s_layers[i].pix;
        total_bytes += d->data_size;
    }
    ESP_LOGI(TAG, "%d/%d layers, %.2f MB PSRAM", loaded, s_n,
             (double)total_bytes / (1024.0 * 1024.0));

    /* 2) build the screen under the LVGL lock */
    if (!hw_display_lock(300)) {
        ESP_LOGE(TAG, "display lock timeout in create");
    }
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    int cx = PF_CANVAS_X - PF_MARGIN;
    int cy = PF_CANVAS_Y - PF_MARGIN;
    for (int i = 0; i < s_n; i++) {
        if (!s_layers[i].pix) continue;
        lv_obj_t *img = lv_image_create(s_screen);
        lv_image_set_src(img, &s_layers[i].dsc);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(img, cx + s_layers[i].off_x, cy + s_layers[i].off_y);
        s_imgs[i] = img;
    }

    if (s_n == 0) {
        lv_obj_t *lbl = lv_label_create(s_screen);
        lv_obj_set_style_text_font(lbl, &noto_font_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_text(lbl, "Parallax\n(no assets)");
        lv_obj_center(lbl);
    }

    /* time / date / battery — same typography + layout as the Minimal face
     * (no pill; the text sits directly over the photo). */
    s_time_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_time_lbl, &ui_font_NotoSansCondensedBold80, 0);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(s_time_lbl, 4, 0);
    lv_label_set_text(s_time_lbl, "--:--");
    lv_obj_align(s_time_lbl, LV_ALIGN_CENTER, 0, -26);

    s_date_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_date_lbl, &ui_font_NotoSansExtraCondensedSemiBold25, 0);
    lv_obj_set_style_text_color(s_date_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(s_date_lbl, "");
    lv_obj_align(s_date_lbl, LV_ALIGN_CENTER, 0, 62);

    s_batt_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_batt_lbl, &ui_font_NotoSansExtraCondensedSemiBold25, 0);
    lv_obj_set_style_text_color(s_batt_lbl, lv_color_white(), 0);
    lv_label_set_text(s_batt_lbl, "--%");
    lv_obj_align(s_batt_lbl, LV_ALIGN_TOP_RIGHT, -14, 10);

    hw_display_unlock();

    /* 3) start the IMU task */
    s_stop = false;
    if (!s_stopped) s_stopped = xSemaphoreCreateBinary();
    xTaskCreate(pf_task, "wf_pfx", PF_TASK_STACK, NULL, PF_TASK_PRIO, &s_task);

    return s_screen;
}

static void pf_destroy(void)
{
    if (s_task) {
        s_stop = true;
        xSemaphoreTake(s_stopped, pdMS_TO_TICKS(PF_STOP_WAIT_MS));
        s_task = NULL;
    }
    if (s_screen) {
        if (hw_display_lock(200)) {
            lv_obj_delete(s_screen);
            hw_display_unlock();
        }
        s_screen = NULL;
    }
    s_time_lbl = NULL;
    s_date_lbl = NULL;
    s_batt_lbl = NULL;
    for (int i = 0; i < PF_MAX_LAYERS; i++) {
        if (s_layers[i].pix) heap_caps_free(s_layers[i].pix);
        s_layers[i].pix = NULL;
        s_imgs[i] = NULL;
    }
    s_n = 0;
    s_aod_dx = 0;
    s_aod_dy = 0;
}

static void pf_update_time(const char *time_str, const char *date_str)
{
    if (s_time_lbl && lv_obj_is_valid(s_time_lbl))
        lv_label_set_text(s_time_lbl, time_str ? time_str : "--:--");
    if (s_date_lbl && lv_obj_is_valid(s_date_lbl))
        lv_label_set_text(s_date_lbl, date_str ? date_str : "");
}

static void pf_update_battery(int pct, bool charging, bool vbus_present)
{
    (void)charging;
    (void)vbus_present;
    if (!s_batt_lbl || !lv_obj_is_valid(s_batt_lbl)) return;
    char b[16];
    snprintf(b, sizeof(b), "%d%%", pct);
    lv_label_set_text(s_batt_lbl, b);
}

/* AOD burn-in prevention: shift the whole composition by (dx, dy) ∈ {0,1}. */
static void pf_aod_pixel_shift(int dx, int dy)
{
    s_aod_dx = dx;
    s_aod_dy = dy;
    pf_reposition();
}

const watch_face_t watch_face_parallax = {
    .name           = "Parallax",
    .id             = WATCH_FACE_PARALLAX,
    .create         = pf_create,
    .destroy        = pf_destroy,
    .update_time    = pf_update_time,
    .update_battery = pf_update_battery,
    .aod_pixel_shift = pf_aod_pixel_shift,
};
