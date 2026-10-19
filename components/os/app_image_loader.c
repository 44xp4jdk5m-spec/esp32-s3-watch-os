/**
 * @file app_image_loader.c
 * @brief SPIFFS → PSRAM loader for pre-decoded raw LVGL images.
 *
 * See app_image_loader.h.  Files are read once into a PSRAM allocation and
 * handed to LVGL as a plain lv_image_dsc_t (same header fields as the
 * original const descriptors), so rendering performance is unchanged.
 */

#include "app_image_loader.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "IMG_LOAD";

lv_image_dsc_t *app_img_load_raw(const char *path, uint32_t w, uint32_t h,
                                 lv_color_format_t cf)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    /* Note: we deliberately do NOT cross-check the file size against
     * w*h*bytes-per-pixel.  lv_color_format_get_bpp() under-reports
     * RGB565A8 (it returns 16 for the colour part, but the buffer is
     * 3 B/px), and indexed formats like I8 carry more data than w*h.
     * The .bin files are byte-exact copies of the original descriptor
     * arrays, so data_size = file size reproduces the exact rendering. */

    uint8_t *data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (!data) {
        ESP_LOGE(TAG, "PSRAM alloc %ld B failed for %s", size, path);
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        ESP_LOGE(TAG, "short read: %s", path);
        heap_caps_free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    lv_image_dsc_t *dsc = heap_caps_malloc(sizeof(lv_image_dsc_t),
                                           MALLOC_CAP_SPIRAM);
    if (!dsc) {
        ESP_LOGE(TAG, "descriptor alloc failed for %s", path);
        heap_caps_free(data);
        return NULL;
    }

    lv_memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf    = cf;
    dsc->header.w     = w;
    dsc->header.h     = h;
    dsc->data_size    = (uint32_t)size;
    dsc->data         = data;

    ESP_LOGI(TAG, "loaded %s (%lux%lu, %ld B)", path,
             (unsigned long)w, (unsigned long)h, size);
    return dsc;
}

void app_img_free(lv_image_dsc_t *dsc)
{
    if (!dsc) {
        return;
    }
    if (dsc->data) {
        heap_caps_free((void *)dsc->data);
    }
    heap_caps_free(dsc);
}
