/**
 * @file screen_capture.c
 * @brief LVGL screen capture to PSRAM.
 *
 * Uses LVGL v9's lv_snapshot_take() to capture the active screen into
 * a draw buffer.  The raw RGB565 data is then wrapped in a BMP header
 * for convenient storage/upload.
 *
 * All large allocations use PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
 * to keep internal DRAM free for LVGL DMA descriptors.
 */

#include "screen_capture.h"
#include "hw_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SCREEN_CAP";

/* ── BMP header helpers ─────────────────────────────────────────────── */

/* Packed struct to avoid padding — BMP header must be byte-packed. */
#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;      /* "BM" */
    uint32_t bfSize;      /* total file size */
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;   /* offset to pixel data */
} bmp_file_header_t;

typedef struct {
    uint32_t biSize;          /* header size = 40 */
    int32_t  biWidth;
    int32_t  biHeight;        /* negative = top-down, positive = bottom-up */
    uint16_t biPlanes;        /* 1 */
    uint16_t biBitCount;      /* 16 for RGB565 */
    uint32_t biCompression;   /* 0 = BI_RGB */
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} bmp_info_header_t;
#pragma pack(pop)

/* ── Capture handle ─────────────────────────────────────────────────── */

struct screen_capture_t {
    uint16_t   width;
    uint16_t   height;
    uint32_t   data_size;      /**< bytes of raw RGB565 pixels */
    uint8_t   *data;           /**< raw RGB565 data (PSRAM) */
};

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t screen_capture_take(screen_capture_t **out_cap)
{
    if (!out_cap) return ESP_ERR_INVALID_ARG;

    /* Must be called under LVGL display lock */
    if (!hw_display_lock(200)) {
        ESP_LOGE(TAG, "Cannot acquire display lock for capture");
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *act_scr = lv_screen_active();
    if (!act_scr) {
        hw_display_unlock();
        return ESP_FAIL;
    }

    /* Take snapshot — returns a draw_buf with the rendered pixels */
    lv_draw_buf_t *snap = lv_snapshot_take(act_scr, LV_COLOR_FORMAT_RGB565);
    if (!snap) {
        ESP_LOGE(TAG, "lv_snapshot_take failed (OOM or invalid obj)");
        hw_display_unlock();
        return ESP_ERR_NO_MEM;
    }

    hw_display_unlock();

    uint32_t w = snap->header.w;
    uint32_t h = snap->header.h;
    uint32_t stride = snap->header.stride;
    const uint8_t *src = (const uint8_t *)snap->data;

    /* Allocate capture struct + pixel data in PSRAM */
    screen_capture_t *cap = (screen_capture_t *)
        heap_caps_calloc(1, sizeof(screen_capture_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cap) {
        ESP_LOGE(TAG, "PSRAM alloc failed for capture struct");
        lv_draw_buf_destroy(snap);
        return ESP_ERR_NO_MEM;
    }

    uint32_t row_bytes = w * 2; /* RGB565 = 2 bytes/pixel */
    uint32_t total_bytes = row_bytes * h;

    cap->data = (uint8_t *)heap_caps_malloc(total_bytes,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cap->data) {
        ESP_LOGE(TAG, "PSRAM alloc failed for %" PRIu32 " bytes", total_bytes);
        heap_caps_free(cap);
        lv_draw_buf_destroy(snap);
        return ESP_ERR_NO_MEM;
    }

    /* Copy pixels row by row (stride may differ from row_bytes) */
    for (uint32_t y = 0; y < h; y++) {
        memcpy(cap->data + y * row_bytes, src + y * stride, row_bytes);
    }

    cap->width     = (uint16_t)w;
    cap->height    = (uint16_t)h;
    cap->data_size = total_bytes;

    lv_draw_buf_destroy(snap);

    ESP_LOGI(TAG, "Captured %dx%d (%" PRIu32 " bytes) in PSRAM",
             cap->width, cap->height, cap->data_size);

    *out_cap = cap;
    return ESP_OK;
}

const uint8_t *screen_capture_get_data(const screen_capture_t *cap)
{
    return cap ? cap->data : NULL;
}

uint32_t screen_capture_get_data_size(const screen_capture_t *cap)
{
    return cap ? cap->data_size : 0;
}

uint16_t screen_capture_get_width(const screen_capture_t *cap)
{
    return cap ? cap->width : 0;
}

uint16_t screen_capture_get_height(const screen_capture_t *cap)
{
    return cap ? cap->height : 0;
}

void screen_capture_free(screen_capture_t *cap)
{
    if (cap) {
        if (cap->data) heap_caps_free(cap->data);
        heap_caps_free(cap);
    }
}

/* ── BMP encoding — 24-bit RGB888 for correct color reproduction ────
 *
 * 16-bit BMP (BI_RGB + biBitCount=16) uses RGB555 layout that causes
 * color shifts with our RGB565 source.  24-bit BMP stores B,G,R bytes
 * directly and is universally supported.  File is ~3x larger but
 * colors are accurate.  PSRAM handles the size.
 *
 * Pixel conversion: RGB565 → RGB888 (5→8 bit expansion) ─────────── */

static inline void _rgb565_to_rgb888(uint16_t rgb565, uint8_t *bgr)
{
    uint8_t r5 = (rgb565 >> 11) & 0x1F;
    uint8_t g6 = (rgb565 >> 5)  & 0x3F;
    uint8_t b5 =  rgb565        & 0x1F;
    /* 5-bit → 8-bit: (val << 3) | (val >> 2) */
    /* 6-bit → 8-bit: (val << 2) | (val >> 4) */
    bgr[0] = (b5 << 3) | (b5 >> 2);  /* Blue  */
    bgr[1] = (g6 << 2) | (g6 >> 4);  /* Green */
    bgr[2] = (r5 << 3) | (r5 >> 2);  /* Red   */
}

esp_err_t screen_capture_encode_bmp(const screen_capture_t *cap,
                                    uint8_t **out_bmp,
                                    uint32_t *out_size)
{
    if (!cap || !out_bmp || !out_size) return ESP_ERR_INVALID_ARG;

    /* 24-bit: 3 bytes per pixel */
    uint32_t row_bytes = cap->width * 3;
    uint32_t stride = (row_bytes + 3) & ~3;
    uint32_t pixel_bytes = stride * cap->height;
    uint32_t header_size = sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t);
    uint32_t total = header_size + pixel_bytes;

    uint8_t *bmp = (uint8_t *)heap_caps_malloc(total,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bmp) {
        ESP_LOGE(TAG, "PSRAM alloc failed for BMP (%" PRIu32 " bytes)", total);
        return ESP_ERR_NO_MEM;
    }

    /* File header */
    bmp_file_header_t *fh = (bmp_file_header_t *)bmp;
    fh->bfType      = 0x4D42;
    fh->bfSize      = total;
    fh->bfReserved1 = 0;
    fh->bfReserved2 = 0;
    fh->bfOffBits   = header_size;

    /* Info header */
    bmp_info_header_t *ih = (bmp_info_header_t *)(bmp + sizeof(bmp_file_header_t));
    ih->biSize          = 40;
    ih->biWidth         = cap->width;
    ih->biHeight        = (int32_t)cap->height;  /* positive = bottom-up (standard) */
    ih->biPlanes        = 1;
    ih->biBitCount      = 24;           /* 24-bit RGB888 */
    ih->biCompression   = 0;            /* BI_RGB */
    ih->biSizeImage     = pixel_bytes;
    ih->biXPelsPerMeter = 2835;
    ih->biYPelsPerMeter = 2835;
    ih->biClrUsed       = 0;
    ih->biClrImportant  = 0;

    /* Convert each pixel: RGB565 → BGR888.
     * Positive height = bottom-up: write rows in reverse order (bottom row first). */
    uint8_t *dst = bmp + header_size;
    const uint8_t *src = cap->data;
    uint16_t pixel;

    for (int32_t y = (int32_t)cap->height - 1; y >= 0; y--) {
        uint32_t src_off = (uint32_t)y * cap->width * 2;
        for (uint32_t x = 0; x < cap->width; x++) {
            /* Read RGB565 (little-endian) */
            pixel = (uint16_t)src[src_off] | ((uint16_t)src[src_off + 1] << 8);
            _rgb565_to_rgb888(pixel, dst + x * 3);
            src_off += 2;
        }
        /* Advance past stride padding */
        dst += stride;
    }

    *out_bmp = bmp;
    *out_size = total;

    ESP_LOGI(TAG, "BMP encoded: %" PRIu32 " bytes (%dx%d 24-bit)", total,
             cap->width, cap->height);
    return ESP_OK;
}
