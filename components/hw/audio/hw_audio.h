/** @file hw_audio.h — Audio codec wrapper (ES8311 + ES7210) */
#pragma once
#include "hw_common.h"
#include "esp_codec_dev.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t hw_audio_init(void);
esp_err_t hw_audio_deinit(void);
esp_err_t hw_audio_set_volume(uint8_t percent);
esp_err_t hw_audio_mute(bool mute);
esp_err_t hw_audio_sleep(void);
esp_err_t hw_audio_wake(void);

/**
 * @brief Get the microphone codec device handle.
 *
 * The mic (ES7210, via the BSP `bsp_audio_codec_microphone_init()` API) is
 * opened once at boot by hw_audio_init() at 16 kHz / mono / 16-bit.  This
 * returns that already-open handle so consumers (e.g. the Noise app) can
 * read PCM without re-opening the shared I2S + codec.
 *
 * @return codec device handle, or NULL if the mic is unavailable.
 */
esp_codec_dev_handle_t hw_audio_get_mic(void);
#ifdef __cplusplus
}
#endif
