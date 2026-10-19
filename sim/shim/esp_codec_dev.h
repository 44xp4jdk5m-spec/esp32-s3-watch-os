/**
 * @file esp_codec_dev.h
 * @brief Host shim for the ESP codec-device component.
 *
 * On the device this comes from `espressif/esp_codec_dev` (managed
 * component).  The simulator only needs the microphone read path; see
 * sim_mocks.c for the synthetic PCM source.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *esp_codec_dev_handle_t;

/* Status codes from esp_codec_dev_types.h */
#define ESP_CODEC_DEV_OK            0
#define ESP_CODEC_DEV_FAIL          (-1)
#define ESP_CODEC_DEV_WRONG_STATE   (-2)
#define ESP_CODEC_DEV_NOT_SUPPORT   (-3)
#define ESP_CODEC_DEV_INVALID_ARG   (-4)

/** Read PCM samples from the codec.  Returns 0 (ESP_OK) on success. */
int esp_codec_dev_read(esp_codec_dev_handle_t codec_dev, void *data,
                       int data_size);

#ifdef __cplusplus
}
#endif
