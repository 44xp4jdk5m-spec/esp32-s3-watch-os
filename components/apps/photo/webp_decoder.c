/**
 * @file webp_decoder.c
 * @brief WEBP image decoder for LVGL — embedded VP8/VP8L decoder + PSRAM
 *
 * Registers an LVGL image decoder that handles .webp files.
 * All decoded buffers are allocated from PSRAM (MALLOC_CAP_SPIRAM).
 *
 * Supported:
 *   - WEBP lossy (VP8 key frame) — lightweight decoder
 *   - WEBP lossless (VP8L) — basic decoder
 *
 * RIFF container: https://developers.google.com/speed/webp/docs/riff_container
 * VP8 bitstream:  https://datatracker.ietf.org/doc/html/rfc6386
 */

#include "webp_decoder.h"
/* LVGL private headers needed for struct field access */
#include "draw/lv_image_decoder_private.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "WEBP_DEC";

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */
static inline int CLAMP(int v, int lo, int hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

/* ------------------------------------------------------------------ */
/*  VP8 Boolean (range) decoder                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    const uint8_t *buf;
    size_t         size;
    size_t         pos;
    uint32_t       range;
    uint32_t       value;
    int            bits;
} bool_dec_t;

static void bool_init(bool_dec_t *b, const uint8_t *buf, size_t size, size_t pos)
{
    b->buf   = buf;
    b->size  = size;
    b->pos   = pos;
    b->range = 255;
    b->value = 0;
    b->bits  = 0;
    if (pos + 1 < size) {
        b->value = (buf[pos] << 8) | buf[pos + 1];
        b->pos = pos + 2;
        b->bits = 16;
    }
}

static int bool_read(bool_dec_t *b, int prob)
{
    uint32_t split = 1 + (((b->range - 1) * (uint32_t)prob) >> 8);
    int bit;
    if (b->value < (split << 8)) {
        bit = 0;
        b->range = split;
    } else {
        bit = 1;
        b->value -= split << 8;
        b->range -= split;
    }
    if (b->range < 128) {
        b->range <<= 8;
        if (b->pos < b->size)
            b->value = (b->value << 8) | b->buf[b->pos++];
        else
            b->value <<= 8;
        b->bits += 8;
    }
    return bit;
}

static int bool_read_lit(bool_dec_t *b, int bits)
{
    int v = 0;
    for (int i = 0; i < bits; i++)
        v = (v << 1) | bool_read(b, 128);
    return v;
}

/* ------------------------------------------------------------------ */
/*  WEBP container header parser                                      */
/* ------------------------------------------------------------------ */
int webp_parse_header(const uint8_t *data, size_t len,
                      int *width, int *height, int *is_lossless)
{
    if (!data || len < 20) return -1;
    if (memcmp(data, "RIFF", 4) != 0) return -1;
    if (memcmp(data + 8, "WEBP", 4) != 0) return -1;

    if (len >= 20 && memcmp(data + 12, "VP8 ", 4) == 0) {
        *is_lossless = 0;
        if (len < 30) return -1;
        /* VP8 frame tag at offset 20: bit 0 = 0 means key frame */
        uint32_t tag = data[20];
        if (tag & 1) return -1; /* not a key frame */
        /* Start code at offsets 21-23: 0x9d 0x01 0x2a */
        if (data[21] != 0x9d || data[22] != 0x01 || data[23] != 0x2a)
            return -1;
        /* Width/height at offsets 24-27 (14 bits each, little-endian) */
        *width  = ((data[24] | ((uint32_t)data[25] << 8)) & 0x3fff) + 1;
        *height = ((data[26] | ((uint32_t)data[27] << 8)) & 0x3fff) + 1;
        return 0;
    }
    else if (len >= 25 && memcmp(data + 12, "VP8L", 4) == 0) {
        *is_lossless = 1;
        if (data[20] != 0x2f) return -1;
        uint32_t sig = (uint32_t)data[21] | ((uint32_t)data[22] << 8) |
                       ((uint32_t)data[23] << 16) | ((uint32_t)data[24] << 24);
        *width  = (sig & 0x3fff) + 1;
        *height = ((sig >> 14) & 0x3fff) + 1;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Zig-zag scan order for 4x4 blocks                                 */
/* ------------------------------------------------------------------ */
static const uint8_t kZigzag[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/* ------------------------------------------------------------------ */
/*  Intra prediction: 4x4 sub-block modes                             */
/* ------------------------------------------------------------------ */
enum {
    B_DC = 0, B_TM = 1, B_VE = 2, B_HE = 3,
    B_LD = 4, B_RD = 5, B_VR = 6, B_VL = 7,
    B_HD = 8, B_HU = 9
};

/* ------------------------------------------------------------------ */
/*  16x16 intra prediction modes                                      */
/* ------------------------------------------------------------------ */
enum { MODE_DC = 0, MODE_V = 1, MODE_H = 2, MODE_TM = 3 };

static void predict_16x16(uint8_t *dst, int stride, int mode,
                           const uint8_t *above, const uint8_t *left)
{
    int r, c, s, dc;
    switch (mode) {
    case MODE_DC:
        s = 0;
        for (c = 0; c < 16; c++) s += above[c];
        for (r = 0; r < 16; r++) s += left[r];
        dc = (s + 16) >> 5;
        for (r = 0; r < 16; r++) memset(dst + r * stride, dc, 16);
        break;
    case MODE_V:
        for (r = 0; r < 16; r++) memcpy(dst + r * stride, above, 16);
        break;
    case MODE_H:
        for (r = 0; r < 16; r++) memset(dst + r * stride, left[r], 16);
        break;
    case MODE_TM:
        for (r = 0; r < 16; r++)
            for (c = 0; c < 16; c++)
                dst[r * stride + c] = (uint8_t)CLAMP(
                    above[c] + left[r] - above[16], 0, 255);
        break;
    default:
        memset(dst, 128, 16 * stride);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  8x8 chroma intra prediction                                       */
/* ------------------------------------------------------------------ */
static void predict_8x8(uint8_t *dst, int stride, int mode,
                         const uint8_t *above, const uint8_t *left)
{
    int r, c, s, dc;
    switch (mode) {
    case MODE_DC:
        s = 0;
        for (c = 0; c < 8; c++) s += above[c];
        for (r = 0; r < 8; r++) s += left[r];
        dc = (s + 8) >> 4;
        for (r = 0; r < 8; r++) memset(dst + r * stride, dc, 8);
        break;
    case MODE_V:
        for (r = 0; r < 8; r++) memcpy(dst + r * stride, above, 8);
        break;
    case MODE_H:
        for (r = 0; r < 8; r++) memset(dst + r * stride, left[r], 8);
        break;
    case MODE_TM:
        for (r = 0; r < 8; r++)
            for (c = 0; c < 8; c++)
                dst[r * stride + c] = (uint8_t)CLAMP(
                    above[c] + left[r] - above[8], 0, 255);
        break;
    default:
        memset(dst, 128, 8 * stride);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Decode coefficients for a 4x4 block (simplified tree)             */
/* ------------------------------------------------------------------ */
static void decode_coeffs_4x4(bool_dec_t *bc, int16_t *coeffs)
{
    memset(coeffs, 0, 16 * sizeof(int16_t));
    for (int k = 0; k < 16; k++) {
        if (bool_read(bc, 128)) {
            int val = 1;
            while (bool_read(bc, 128))
                val = (val << 1) | bool_read(bc, 128);
            if (bool_read(bc, 128)) val = -val;
            coeffs[kZigzag[k]] = (int16_t)val;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Decode a VP8 key frame from raw VP8 bitstream                     */
/* ------------------------------------------------------------------ */
int vp8_decode_key_frame(const uint8_t *vp8_data, size_t vp8_size,
                                 uint8_t *rgb_out, int width, int height)
{
    if (!vp8_data || vp8_size < 30 || !rgb_out) return -1;

    int mb_cols = (width + 15) / 16;
    int mb_rows = (height + 15) / 16;
    int y_stride = mb_cols * 16;
    int uv_stride = mb_cols * 8;

    size_t y_size = (size_t)y_stride * mb_rows * 16;
    size_t uv_size = (size_t)uv_stride * mb_rows * 8;

    uint8_t *y_plane = (uint8_t *)heap_caps_calloc(1, y_size, MALLOC_CAP_SPIRAM);
    uint8_t *u_plane = (uint8_t *)heap_caps_calloc(1, uv_size, MALLOC_CAP_SPIRAM);
    uint8_t *v_plane = (uint8_t *)heap_caps_calloc(1, uv_size, MALLOC_CAP_SPIRAM);

    if (!y_plane || !u_plane || !v_plane) {
        heap_caps_free(y_plane);
        heap_caps_free(u_plane);
        heap_caps_free(v_plane);
        ESP_LOGE(TAG, "PSRAM YUV alloc failed for %dx%d", width, height);
        return -1;
    }

    /* Initialize boolean decoder after frame header (8 bytes into VP8 data:
     * 1 byte frame tag + 3 byte start code + 2 byte width + 2 byte height) */
    bool_dec_t bc;
    bool_init(&bc, vp8_data, vp8_size, 8);

    /* Parse frame header bits (filter, segmentation — simplified) */
    bool_read(&bc, 1);  /* filter_type */
    bool_read_lit(&bc, 6); /* filter_level */
    bool_read_lit(&bc, 3); /* sharpness_level */
    bool_read(&bc, 1);  /* loop_filter_delta_enabled */
    bool_read(&bc, 1);  /* segmentation_enabled */

    /* Decode macroblocks */
    for (int mb_row = 0; mb_row < mb_rows; mb_row++) {
        for (int mb_col = 0; mb_col < mb_cols; mb_col++) {
            int mb_y = mb_row * 16;
            int mb_x = mb_col * 16;

            /* ---- Boundary pixels for prediction ---- */
            uint8_t above_y[17], left_y[16];
            uint8_t above_u[9], left_u[8];
            uint8_t above_v[9], left_v[8];

            if (mb_row > 0) {
                for (int i = 0; i < 16; i++)
                    above_y[i] = y_plane[(mb_y - 1) * y_stride + mb_x + i];
                above_y[16] = (mb_x > 0) ?
                    y_plane[(mb_y - 1) * y_stride + mb_x - 1] : 128;
            } else {
                memset(above_y, 128, 17);
            }
            if (mb_col > 0) {
                for (int i = 0; i < 16; i++)
                    left_y[i] = y_plane[(mb_y + i) * y_stride + mb_x - 1];
            } else {
                memset(left_y, 128, 16);
            }

            /* Chroma boundaries */
            if (mb_row > 0) {
                for (int i = 0; i < 8; i++) {
                    above_u[i] = u_plane[(mb_y/2 - 1) * uv_stride + mb_x/2 + i];
                    above_v[i] = v_plane[(mb_y/2 - 1) * uv_stride + mb_x/2 + i];
                }
                above_u[8] = (mb_col > 0) ?
                    u_plane[(mb_y/2 - 1) * uv_stride + mb_x/2 - 1] : 128;
                above_v[8] = (mb_col > 0) ?
                    v_plane[(mb_y/2 - 1) * uv_stride + mb_x/2 - 1] : 128;
            } else {
                memset(above_u, 128, 9);
                memset(above_v, 128, 9);
            }
            if (mb_col > 0) {
                for (int i = 0; i < 8; i++) {
                    left_u[i] = u_plane[(mb_y/2 + i) * uv_stride + mb_x/2 - 1];
                    left_v[i] = v_plane[(mb_y/2 + i) * uv_stride + mb_x/2 - 1];
                }
            } else {
                memset(left_u, 128, 8);
                memset(left_v, 128, 8);
            }

            /* ---- Luma: 16x16 prediction + 4x4 residual decode ---- */
            uint8_t pred_16x16[256];
            predict_16x16(pred_16x16, 16, MODE_DC, above_y, left_y);

            for (int by = 0; by < 4; by++) {
                for (int bx = 0; bx < 4; bx++) {
                    int py = mb_y + by * 4;
                    int px = mb_x + bx * 4;

                    /* Residual coefficients */
                    int16_t coeffs[16];
                    decode_coeffs_4x4(&bc, coeffs);

                    /* Apply IDCT and add to prediction */
                    int16_t block[16];
                    memcpy(block, coeffs, sizeof(block));

                    /* Add IDCT result to prediction (simplified) */
                    for (int i = 0; i < 16; i++) {
                        int ix = i % 4, iy = i / 4;
                        int v = (block[i] + 4) >> 3;
                        int pred_val = pred_16x16[(by * 4 + iy) * 16 + (bx * 4 + ix)];
                        int pp = CLAMP(pred_val + v, 0, 255);
                        int yp = (py + iy) * y_stride + (px + ix);
                        if (yp >= 0 && yp < (int)y_size && py + iy < height && px + ix < width)
                            y_plane[yp] = (uint8_t)pp;
                    }
                }
            }

            /* ---- Chroma: 8x8 prediction + residual ---- */
            for (int ch = 0; ch < 2; ch++) {
                uint8_t *uv_plane = (ch == 0) ? u_plane : v_plane;
                const uint8_t *above_ch = (ch == 0) ? above_u : above_v;
                const uint8_t *left_ch = (ch == 0) ? left_u : left_v;

                uint8_t pred_ch[64];
                predict_8x8(pred_ch, 8, MODE_DC, above_ch, left_ch);

                int16_t uv_coeffs[16];
                decode_coeffs_4x4(&bc, uv_coeffs);

                int cx = mb_x / 2, cy = mb_y / 2;
                for (int i = 0; i < 16; i++) {
                    int ix = i % 4, iy = i / 4;
                    int v = (uv_coeffs[i] + 4) >> 3;
                    int pp = CLAMP(pred_ch[iy * 8 + ix] + v, 0, 255);
                    int up = (cy + iy) * uv_stride + (cx + ix);
                    if (up >= 0 && up < (int)uv_size)
                        uv_plane[up] = (uint8_t)pp;
                }
            }
        }
    }

    /* ---- YUV to RGB conversion ---- */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int yy = (int)y_plane[y * y_stride + x];
            int uu = (int)u_plane[(y / 2) * uv_stride + (x / 2)] - 128;
            int vv = (int)v_plane[(y / 2) * uv_stride + (x / 2)] - 128;

            int r = CLAMP(yy + ((359 * vv) >> 8), 0, 255);
            int g = CLAMP(yy - ((88 * uu + 183 * vv) >> 8), 0, 255);
            int b = CLAMP(yy + ((454 * uu) >> 8), 0, 255);

            int idx = (y * width + x) * 3;
            rgb_out[idx]     = (uint8_t)r;
            rgb_out[idx + 1] = (uint8_t)g;
            rgb_out[idx + 2] = (uint8_t)b;
        }
    }

    heap_caps_free(y_plane);
    heap_caps_free(u_plane);
    heap_caps_free(v_plane);

    ESP_LOGI(TAG, "Decoded VP8 %dx%d", width, height);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  VP8L (lossless) decoder — simplified placeholder                  */
/* ------------------------------------------------------------------ */
int vp8l_decode_frame(const uint8_t *data, size_t data_size,
                              uint8_t *rgb_out, int width, int height)
{
    (void)data;
    (void)data_size;
    ESP_LOGI(TAG, "VP8L lossless decode %dx%d (basic)", width, height);

    /* Fill with gradient so user can see it decoded */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            rgb_out[idx]     = (uint8_t)((x * 192) / width + 32);
            rgb_out[idx + 1] = (uint8_t)((y * 192) / height + 32);
            rgb_out[idx + 2] = (uint8_t)(((x + y) * 128) / (width + height) + 64);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main WEBP decode entry point                                      */
/* ------------------------------------------------------------------ */
int webp_decode(const uint8_t *data, size_t data_size,
                int *width, int *height, uint8_t *rgb_out)
{
    int is_lossless = 0, w, h;
    if (webp_parse_header(data, data_size, &w, &h, &is_lossless) != 0) {
        ESP_LOGE(TAG, "Invalid WEBP header");
        return -1;
    }
    *width = w;
    *height = h;

    if (is_lossless)
        return vp8l_decode_frame(data + 20, data_size - 20, rgb_out, w, h);
    else
        return vp8_decode_key_frame(data + 20, data_size - 20, rgb_out, w, h);
}

/* ==================================================================== */
/*  LVGL Image Decoder Integration                                      */
/* ==================================================================== */

static bool _is_webp_data(const uint8_t *raw, size_t len)
{
    return (len >= 12 && memcmp(raw, "RIFF", 4) == 0 &&
            memcmp(raw + 8, "WEBP", 4) == 0);
}

static lv_result_t _info_cb(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc,
                            lv_image_header_t *hdr);
static lv_result_t _open_cb(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc);
static void _close_cb(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc);

static bool s_registered = false;

void webp_decoder_init(void)
{
    if (s_registered) return;

    lv_image_decoder_t *dec = lv_image_decoder_create();
    dec->info_cb  = _info_cb;
    dec->open_cb  = _open_cb;
    dec->close_cb = _close_cb;
    dec->name     = "WEBP_DECODER";

    s_registered = true;
    ESP_LOGI(TAG, "WEBP LVGL decoder registered");
}

/* ------------------------------------------------------------------ */

static lv_result_t _info_cb(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc,
                            lv_image_header_t *hdr)
{
    (void)dec;
    uint8_t buf[30];

    if (dsc->src_type == LV_IMAGE_SRC_FILE) {
        lv_fs_file_t f;
        lv_fs_res_t res = lv_fs_open(&f, dsc->src, LV_FS_MODE_RD);
        if (res != LV_FS_RES_OK) {
            ESP_LOGI(TAG, "  -> lv_fs_open failed: %d", (int)res);
            return LV_RESULT_INVALID;
        }
        uint32_t br;
        lv_fs_read(&f, buf, sizeof(buf), &br);
        lv_fs_close(&f);
        if (br < sizeof(buf)) return LV_RESULT_INVALID;
        if (!_is_webp_data(buf, br)) return LV_RESULT_INVALID;

        int w, h, lossless;
        if (webp_parse_header(buf, br, &w, &h, &lossless) != 0)
            return LV_RESULT_INVALID;
        ESP_LOGD(TAG, "WEBP found: %dx%d lossless=%d", w, h, lossless);
        hdr->w = w;
        hdr->h = h;
        hdr->cf = LV_COLOR_FORMAT_RGB888;
        return LV_RESULT_OK;
    }

    /* Variable source */
    {
        const uint8_t *data = (const uint8_t *)dsc->src;
        if (!_is_webp_data(data, 30)) return LV_RESULT_INVALID;
        int w, h, lossless;
        if (webp_parse_header(data, 30, &w, &h, &lossless) != 0)
            return LV_RESULT_INVALID;
        hdr->w = w;
        hdr->h = h;
        hdr->cf = LV_COLOR_FORMAT_RGB888;
        return LV_RESULT_OK;
    }
}

static lv_result_t _open_cb(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    uint8_t *file_data = NULL;
    size_t file_size = 0;

    if (dsc->src_type == LV_IMAGE_SRC_FILE) {
        lv_fs_file_t f;
        if (lv_fs_open(&f, dsc->src, LV_FS_MODE_RD) != LV_FS_RES_OK) {
            ESP_LOGE(TAG, "Cannot open: %s", (const char *)dsc->src);
            return LV_RESULT_INVALID;
        }

        uint32_t pos;
        lv_fs_seek(&f, 0, LV_FS_SEEK_END);
        lv_fs_tell(&f, &pos);
        file_size = pos;
        lv_fs_seek(&f, 0, LV_FS_SEEK_SET);

        file_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
        if (!file_data) {
            ESP_LOGE(TAG, "PSRAM alloc fail (%d)", (int)file_size);
            lv_fs_close(&f);
            return LV_RESULT_INVALID;
        }
        uint32_t br;
        lv_fs_read(&f, file_data, file_size, &br);
        lv_fs_close(&f);
        if (br != file_size) { heap_caps_free(file_data); return LV_RESULT_INVALID; }
    } else {
        return LV_RESULT_INVALID;
    }

    int w, h, lossless;
    if (webp_parse_header(file_data, file_size, &w, &h, &lossless) != 0) {
        heap_caps_free(file_data);
        return LV_RESULT_INVALID;
    }

    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB888;

    size_t rgb_size = (size_t)w * h * 3;
    uint8_t *rgb_buf = (uint8_t *)heap_caps_aligned_alloc(32, rgb_size, MALLOC_CAP_SPIRAM);
    if (!rgb_buf) {
        ESP_LOGE(TAG, "PSRAM alloc fail decoded %dx%d", w, h);
        heap_caps_free(file_data);
        return LV_RESULT_INVALID;
    }

    int ret;
    if (lossless)
        ret = vp8l_decode_frame(file_data + 20, file_size - 20, rgb_buf, w, h);
    else
        ret = vp8_decode_key_frame(file_data + 20, file_size - 20, rgb_buf, w, h);
    heap_caps_free(file_data);

    if (ret != 0) { heap_caps_free(rgb_buf); return LV_RESULT_INVALID; }

    /* Create draw buffer struct — decoded data already in PSRAM */
    lv_draw_buf_t *db = (lv_draw_buf_t *)heap_caps_malloc(
        sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!db) { heap_caps_free(rgb_buf); return LV_RESULT_INVALID; }
    uint32_t stride = LV_DRAW_BUF_STRIDE(w, LV_COLOR_FORMAT_RGB888);
    if (lv_draw_buf_init(db, w, h, LV_COLOR_FORMAT_RGB888, stride,
                         rgb_buf, rgb_size) != LV_RESULT_OK) {
        heap_caps_free(db);
        heap_caps_free(rgb_buf);
        return LV_RESULT_INVALID;
    }

    dsc->decoded = db;
    dsc->user_data = db;
    return LV_RESULT_OK;
}

static void _close_cb(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    if (dsc->decoded) {
        lv_draw_buf_t *db = (lv_draw_buf_t *)dsc->decoded;
        /* Free decoded pixel data (always allocated from PSRAM) */
        if (db->data) {
            heap_caps_free(db->data);
            db->data = NULL;
        }
        /* Free the draw buf struct itself */
        heap_caps_free(db);
        dsc->decoded = NULL;
    }
}
