/**
 * @file app_image_loader.h
 * @brief Load pre-decoded raw LVGL images from SPIFFS into PSRAM on demand.
 *
 * Large watch-face images (flux digits, the classic face's earth + charge
 * icons) and the Activity app icons used to be compiled into the firmware
 * as const arrays.  With CONFIG_SPIRAM_RODATA=y they occupied PSRAM for
 * the whole session.  They are now shipped as raw pixel .bin files under
 * /spiffs/usr/share/watchfaces/ and /spiffs/usr/share/activity/ and are
 * loaded here into PSRAM only while their screen is displayed, then freed.
 *
 * The .bin files are produced by tools/export_image_bins.py.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load a pre-decoded raw LVGL image from SPIFFS into PSRAM.
 *
 * @param path  /spiffs path to the .bin (raw pixel data, no header).
 * @param w     image width  in px (must match the exported descriptor).
 * @param h     image height in px.
 * @param cf    LVGL color format (LV_COLOR_FORMAT_A8, ..._NATIVE_WITH_ALPHA,
 *              ...).  Must match how the .bin was exported.
 *
 * @return a heap-allocated lv_image_dsc_t whose .data points to a PSRAM
 *         buffer, or NULL on any failure.  Release with app_img_free().
 */
lv_image_dsc_t *app_img_load_raw(const char *path, uint32_t w, uint32_t h,
                                 lv_color_format_t cf);

/**
 * @brief Free a descriptor returned by app_img_load_raw() (NULL-safe).
 */
void app_img_free(lv_image_dsc_t *dsc);

#ifdef __cplusplus
}
#endif
