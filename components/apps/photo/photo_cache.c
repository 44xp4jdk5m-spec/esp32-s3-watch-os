/**
 * @file photo_cache.c
 * @brief SD card thumbnail cache implementation.
 *
 * Strategy:
 *   1. First access to a thumbnail → decode full image, resize to
 *      thumbnail size, save raw RGB565 data to SD card.
 *   2. Subsequent accesses → read raw data from SD card into PSRAM,
 *      create an lv_draw_buf_t, and return it — no decoding needed.
 *
 * For BMP files captured with old firmware (negative biHeight), the
 * header is corrected in-place before passing to LVGL's decoder.
 */

#include "photo_cache.h"
#include "webp_decoder.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "draw/lv_image_decoder_private.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static const char *TAG = "PHOTO_CACHE";

/* ── Cache file format ─────────────────────────────────────────────── */
#define CACHE_MAGIC  0x48434143  /* "CACH" */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;      /* CACHE_MAGIC */
    uint16_t width;
    uint16_t height;
    /* raw RGB565 data follows immediately */
} cache_file_hdr_t;
#pragma pack(pop)

/* ── Cache path helpers ────────────────────────────────────────────── */

void photo_cache_get_path(const char *src_path, char *out_path, size_t out_sz)
{
    /* Map "S:/shots/foo.bmp" → "/sdcard/shots/.cache/foo.bmp.cch" */
    const char *prefix = "S:/shots/";
    const char *fname = src_path;

    if (strncmp(src_path, prefix, strlen(prefix)) == 0)
        fname = src_path + strlen(prefix);
    else if (strncmp(src_path, "S:/", 3) == 0)
        fname = src_path + 3;

    snprintf(out_path, out_sz, "/sdcard/shots/.cache/%s.cch", fname);
}

bool photo_cache_exists(const char *src_path)
{
    char cache_path[256];
    photo_cache_get_path(src_path, cache_path, sizeof(cache_path));

    struct stat st;
    return (stat(cache_path, &st) == 0 && st.st_size > (int)sizeof(cache_file_hdr_t));
}

/* ── Nearest-neighbour downscale ──────────────────────────────────── */

static void _scale_down_rgb565(const uint8_t *src, int src_w, int src_h,
                                uint8_t *dst, int dst_w, int dst_h)
{
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = (dy * src_h) / dst_h;
        if (sy >= src_h) sy = src_h - 1;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = (dx * src_w) / dst_w;
            if (sx >= src_w) sx = src_w - 1;
            int src_idx = (sy * src_w + sx) * 2;
            int dst_idx = (dy * dst_w + dx) * 2;
            dst[dst_idx]     = src[src_idx];
            dst[dst_idx + 1] = src[src_idx + 1];
        }
    }
}

/* ── BMP biHeight fix for old capture files ────────────────────────── */

/**
 * LVGL's BMP decoder cannot handle negative biHeight (top-down DIB).
 * Old firmware saved BMP with negative biHeight.  This function
 * detects that case and fixes the header + reorders rows in-place.
 */
static esp_err_t _fix_bmp_header(uint8_t *data, size_t len)
{
    if (len < 54) return ESP_FAIL;
    if (data[0] != 'B' || data[1] != 'M') return ESP_FAIL;

    /* Read biHeight at offset 22 (signed 32-bit) */
    int32_t biHeight;
    memcpy(&biHeight, data + 22, sizeof(biHeight));

    if (biHeight >= 0) return ESP_OK; /* already bottom-up, fine */

    uint32_t h = (uint32_t)(-biHeight);
    uint16_t bpp;
    memcpy(&bpp, data + 28, sizeof(bpp));

    if (bpp != 24) return ESP_FAIL; /* only handle 24-bit BMP */

    /* Fix header: make biHeight positive */
    biHeight = (int32_t)h;
    memcpy(data + 22, &biHeight, sizeof(biHeight));

    /* Reverse pixel rows: old firmware wrote top-down, need bottom-up */
    uint32_t w;
    memcpy(&w, data + 18, sizeof(w));

    uint32_t row_bytes = w * 3;
    uint32_t stride = (row_bytes + 3) & ~3;
    uint32_t pixel_off;
    memcpy(&pixel_off, data + 10, sizeof(pixel_off));
    uint8_t *pixels = data + pixel_off;

    /* Allocate temp row buffer from stack or PSRAM */
    uint8_t *tmp = malloc(stride);
    if (!tmp) return ESP_FAIL;

    for (uint32_t y = 0; y < h / 2; y++) {
        uint8_t *top = pixels + y * stride;
        uint8_t *bot = pixels + (h - 1 - y) * stride;
        memcpy(tmp, top, stride);
        memcpy(top, bot, stride);
        memcpy(bot, tmp, stride);
    }
    free(tmp);

    ESP_LOGI(TAG, "Fixed BMP negative biHeight (%" PRId32 " → %" PRIu32 ")", 
             -biHeight, h);
    return ESP_OK;
}

/* ── RGB888 → RGB565 inline conversion ────────────────────────────── */

static lv_draw_buf_t *_rgb888_to_rgb565(const lv_draw_buf_t *src)
{
    if (!src || src->header.cf != LV_COLOR_FORMAT_RGB888) return NULL;

    int w = src->header.w;
    int h = src->header.h;
    size_t out_size = (size_t)w * h * 2;
    uint32_t out_stride = LV_DRAW_BUF_STRIDE(w, LV_COLOR_FORMAT_RGB565);

    uint8_t *pixels = (uint8_t *)heap_caps_aligned_alloc(32, out_size, MALLOC_CAP_SPIRAM);
    if (!pixels) return NULL;

    const uint8_t *in = src->data;
    for (int i = 0; i < w * h; i++) {
        uint8_t r = in[i * 3];
        uint8_t g = in[i * 3 + 1];
        uint8_t b = in[i * 3 + 2];
        uint16_t p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        pixels[i * 2]     = p & 0xFF;
        pixels[i * 2 + 1] = (p >> 8) & 0xFF;
    }

    lv_draw_buf_t *db = (lv_draw_buf_t *)heap_caps_malloc(
        sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM);
    if (!db) { heap_caps_free(pixels); return NULL; }

    lv_draw_buf_init(db, w, h, LV_COLOR_FORMAT_RGB565, out_stride,
                     pixels, out_size);
    return db;
}

/* ── Direct 24-bit BMP decoder (bypasses LVGL decoder chain) ──────── */

static lv_draw_buf_t *_decode_bmp_direct(const uint8_t *data, size_t len)
{
    if (len < 54 || data[0] != 'B' || data[1] != 'M') return NULL;

    /* Read BMP header fields */
    uint32_t pixel_off, w, h;
    int32_t biHeight;
    uint16_t bpp;
    memcpy(&pixel_off, data + 10, sizeof(pixel_off));
    memcpy(&w, data + 18, sizeof(w));
    memcpy(&biHeight, data + 22, sizeof(biHeight));
    memcpy(&bpp, data + 28, sizeof(bpp));

    if (bpp != 24) {
        ESP_LOGW(TAG, "BMP not 24-bit (%d bpp)", bpp);
        return NULL;
    }

    bool top_down = (biHeight < 0);
    h = (uint32_t)(top_down ? -biHeight : biHeight);

    /* Skip color table if present */
    uint32_t colors_used = 0;
    memcpy(&colors_used, data + 46, sizeof(colors_used));
    if (colors_used == 0 && bpp <= 8) colors_used = 1 << bpp;
    uint32_t bmp_pixel_off = pixel_off + colors_used * 4; /* color table is BGRA */

    /* Row stride in BMP (4-byte aligned) */
    uint32_t row_bytes = w * 3;
    uint32_t bmp_stride = (row_bytes + 3) & ~3;

    /* Allocate RGB565 output */
    size_t out_size = (size_t)w * h * 2;
    uint32_t out_stride = LV_DRAW_BUF_STRIDE(w, LV_COLOR_FORMAT_RGB565);
    uint8_t *pixels = (uint8_t *)heap_caps_aligned_alloc(32, out_size, MALLOC_CAP_SPIRAM);
    if (!pixels) return NULL;

    /* Convert BGR888 → RGB565 row by row */
    for (uint32_t y = 0; y < h; y++) {
        uint32_t src_row = top_down ? y : (h - 1 - y);
        const uint8_t *row = data + bmp_pixel_off + src_row * bmp_stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t b = row[x * 3];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            uint16_t p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            pixels[(y * w + x) * 2]     = p & 0xFF;
            pixels[(y * w + x) * 2 + 1] = (p >> 8) & 0xFF;
        }
    }

    lv_draw_buf_t *db = (lv_draw_buf_t *)heap_caps_malloc(
        sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM);
    if (!db) { heap_caps_free(pixels); return NULL; }

    lv_draw_buf_init(db, w, h, LV_COLOR_FORMAT_RGB565, out_stride,
                     pixels, out_size);
    return db;
}

/* ── Decode image (WEBP / BMP / JPG / PNG / GIF) ──────────────────── */

static lv_draw_buf_t *_decode_image(const char *src_path)
{
    const char *ext = strrchr(src_path, '.');
    if (!ext) return NULL;

    /* ── WEBP: use our custom decoder ── */
    if (strcasecmp(ext, ".webp") == 0) {
        /* Read file into PSRAM via LVGL FS */
        lv_fs_file_t f;
        if (lv_fs_open(&f, src_path, LV_FS_MODE_RD) != LV_FS_RES_OK)
            return NULL;

        uint32_t fsize;
        lv_fs_seek(&f, 0, LV_FS_SEEK_END);
        lv_fs_tell(&f, &fsize);
        lv_fs_seek(&f, 0, LV_FS_SEEK_SET);

        uint8_t *fdata = (uint8_t *)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
        if (!fdata) { lv_fs_close(&f); return NULL; }

        uint32_t br;
        lv_fs_read(&f, fdata, fsize, &br);
        lv_fs_close(&f);
        if (br != fsize) { heap_caps_free(fdata); return NULL; }

        /* Parse header and decode */
        int w, h, lossless;
        if (webp_parse_header(fdata, fsize, &w, &h, &lossless) != 0) {
            ESP_LOGW(TAG, "WEBP header parse failed: %s", src_path);
            heap_caps_free(fdata);
            return NULL;
        }

        size_t rgb_size = (size_t)w * h * 3;
        uint8_t *rgb = (uint8_t *)heap_caps_aligned_alloc(32, rgb_size, MALLOC_CAP_SPIRAM);
        if (!rgb) { heap_caps_free(fdata); return NULL; }

        int ret;
        if (lossless)
            ret = vp8l_decode_frame(fdata + 20, fsize - 20, rgb, w, h);
        else
            ret = vp8_decode_key_frame(fdata + 20, fsize - 20, rgb, w, h);
        heap_caps_free(fdata);

        if (ret != 0) { heap_caps_free(rgb); return NULL; }

        /* Create RGB888 draw_buf, then convert to RGB565 */
        lv_draw_buf_t *tmp = (lv_draw_buf_t *)heap_caps_malloc(
            sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM);
        if (!tmp) { heap_caps_free(rgb); return NULL; }

        uint32_t rgb_stride = LV_DRAW_BUF_STRIDE(w, LV_COLOR_FORMAT_RGB888);
        lv_draw_buf_init(tmp, w, h, LV_COLOR_FORMAT_RGB888, rgb_stride,
                         rgb, rgb_size);

        lv_draw_buf_t *result = _rgb888_to_rgb565(tmp);
        lv_draw_buf_destroy(tmp);
        return result;
    }

    /* ── BMP: direct 24-bit decoder (no LVGL dependency) ── */
    if (strcasecmp(ext, ".bmp") == 0) {
        /* Map "S:/shots/x.bmp" → "/sdcard/shots/x.bmp" */
        const char *fs_path = src_path;
        char mapped_path[128];
        if (strncmp(src_path, "S:/", 3) == 0) {
            snprintf(mapped_path, sizeof(mapped_path), "/sdcard%s", src_path + 2);
            fs_path = mapped_path;
        }

        FILE *fp = fopen(fs_path, "rb");
        if (!fp) return NULL;
        fseek(fp, 0, SEEK_END);
        long flen = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (flen < 54) { fclose(fp); return NULL; }

        uint8_t *fdata = (uint8_t *)heap_caps_malloc(flen, MALLOC_CAP_SPIRAM);
        if (!fdata) { fclose(fp); return NULL; }
        if (fread(fdata, 1, flen, fp) != (size_t)flen) {
            heap_caps_free(fdata); fclose(fp); return NULL;
        }
        fclose(fp);

        /* Fix negative biHeight if present (makes bottom-up, standard) */
        _fix_bmp_header(fdata, (size_t)flen);

        /* Decode directly */
        lv_draw_buf_t *result = _decode_bmp_direct(fdata, (size_t)flen);
        heap_caps_free(fdata);
        return result;
    }

    /* ── JPG, PNG, GIF: decode via LVGL file-path decoder ── */
    {
        ESP_LOGD(TAG, "Decoding via LVGL: %s", src_path);
        lv_image_decoder_dsc_t dsc;
        lv_result_t res = lv_image_decoder_open(&dsc, src_path, NULL);
        if (res != LV_RESULT_OK) {
            ESP_LOGW(TAG, "LVGL decoder open failed (%d): %s", (int)res, src_path);
            return NULL;
        }

        lv_draw_buf_t *decoded = (lv_draw_buf_t *)dsc.decoded;
        if (!decoded || !decoded->data) {
            ESP_LOGW(TAG, "LVGL decoder returned no data: %s", src_path);
            lv_image_decoder_close(&dsc);
            return NULL;
        }

        lv_draw_buf_t *clone = lv_draw_buf_dup(decoded);
        lv_image_decoder_close(&dsc);

        if (!clone) return NULL;

        /* Convert to RGB565 if needed */
        if (clone->header.cf != LV_COLOR_FORMAT_RGB565) {
            lv_draw_buf_t *conv = _rgb888_to_rgb565(clone);
            lv_draw_buf_destroy(clone);
            return conv;
        }
        return clone;
    }
}

/* ── Generate thumbnail cache ──────────────────────────────────────── */

lv_draw_buf_t *photo_cache_generate(const char *src_path,
                                     int thumb_w, int thumb_h)
{
    /* Decode full-size image */
    lv_draw_buf_t *full = _decode_image(src_path);
    if (!full) {
        ESP_LOGW(TAG, "Failed to decode: %s", src_path);
        return NULL;
    }

    int src_w = (int)full->header.w;
    int src_h = (int)full->header.h;

    /* Compute thumbnail size maintaining aspect ratio */
    float scale = (float)thumb_w / src_w;
    int out_w = thumb_w;
    int out_h = (int)(src_h * scale);
    if (out_h < 1) out_h = 1;
    if (out_h > thumb_h * 2) out_h = thumb_h * 2; /* cap at 2x height */

    /* Allocate resized buffer */
    size_t out_size = (size_t)out_w * out_h * 2;
    uint8_t *scaled = (uint8_t *)heap_caps_aligned_alloc(32, out_size, MALLOC_CAP_SPIRAM);
    if (!scaled) {
        lv_draw_buf_destroy(full);
        return NULL;
    }

    /* Scale down (source is RGB565) */
    _scale_down_rgb565((const uint8_t *)full->data, src_w, src_h,
                        scaled, out_w, out_h);
    lv_draw_buf_destroy(full);

    /* Save to SD card */
    char cache_path[256];
    photo_cache_get_path(src_path, cache_path, sizeof(cache_path));

    /* Ensure cache directory exists */
    char dir[256];
    strncpy(dir, cache_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }

    FILE *fp = fopen(cache_path, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "Cannot write cache: %s", cache_path);
        heap_caps_free(scaled);
        return NULL;
    }

    cache_file_hdr_t hdr;
    hdr.magic  = CACHE_MAGIC;
    hdr.width  = (uint16_t)out_w;
    hdr.height = (uint16_t)out_h;

    fwrite(&hdr, sizeof(hdr), 1, fp);
    fwrite(scaled, 1, out_size, fp);
    fclose(fp);

    ESP_LOGI(TAG, "Cached %s → %s (%dx%d RGB565)",
             src_path, cache_path, out_w, out_h);

    /* Create and return draw_buf from the scaled data */
    lv_draw_buf_t *db = (lv_draw_buf_t *)heap_caps_malloc(
        sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM);
    if (!db) { heap_caps_free(scaled); return NULL; }

    uint32_t stride = LV_DRAW_BUF_STRIDE(out_w, LV_COLOR_FORMAT_RGB565);
    if (lv_draw_buf_init(db, out_w, out_h, LV_COLOR_FORMAT_RGB565, stride,
                         scaled, out_size) != LV_RESULT_OK) {
        heap_caps_free(db);
        heap_caps_free(scaled);
        return NULL;
    }
    return db;
}

/* ── Load cached thumbnail ─────────────────────────────────────────── */

lv_draw_buf_t *photo_cache_load(const char *src_path)
{
    char cache_path[256];
    photo_cache_get_path(src_path, cache_path, sizeof(cache_path));

    FILE *fp = fopen(cache_path, "rb");
    if (!fp) return NULL;

    cache_file_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        return NULL;
    }

    if (hdr.magic != CACHE_MAGIC) {
        fclose(fp);
        return NULL;
    }

    uint16_t w = hdr.width;
    uint16_t h = hdr.height;
    size_t px_size = (size_t)w * h * 2;
    uint32_t stride = LV_DRAW_BUF_STRIDE(w, LV_COLOR_FORMAT_RGB565);

    uint8_t *pixels = (uint8_t *)heap_caps_aligned_alloc(32, px_size, MALLOC_CAP_SPIRAM);
    if (!pixels) { fclose(fp); return NULL; }

    if (fread(pixels, 1, px_size, fp) != px_size) {
        heap_caps_free(pixels);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    lv_draw_buf_t *db = (lv_draw_buf_t *)heap_caps_malloc(
        sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM);
    if (!db) { heap_caps_free(pixels); return NULL; }

    if (lv_draw_buf_init(db, w, h, LV_COLOR_FORMAT_RGB565, stride,
                         pixels, px_size) != LV_RESULT_OK) {
        heap_caps_free(db);
        heap_caps_free(pixels);
        return NULL;
    }

    return db;
}
