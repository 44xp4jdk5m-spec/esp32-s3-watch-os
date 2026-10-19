/** @file hw_audio.c */
#include "hw_audio.h"
#include "hw_config.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

static const char *TAG = "HW_AUDIO";
static esp_codec_dev_handle_t s_spk = NULL, s_mic = NULL;
static bool s_ok = false;

/* Sample configs + volume remembered so audio can be powered down in standby
 * and reopened on a real wake.  Closing the codec devices (ES8311 + ES7210)
 * drops the audio current from ~8 mA (ES8311 normal operation) to ~0 uA
 * (power-down) while the display is off. */
static esp_codec_dev_sample_info_t s_spk_fs;
static esp_codec_dev_sample_info_t s_mic_fs;
static int s_spk_vol = 60;

esp_err_t hw_audio_init(void)
{
    if (s_ok) return ESP_OK;

    i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = HW_I2S_MCLK,
            .bclk = HW_I2S_SCLK,
            .ws   = HW_I2S_LCLK,
            .dout = HW_I2S_DOUT,
            .din  = HW_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    esp_err_t ret = bsp_audio_init(&i2s_cfg);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "Audio I2S init skipped (err=0x%x)", ret); return ret; }

    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk) {
        s_spk_fs = (esp_codec_dev_sample_info_t){ .bits_per_sample = 16, .channel = 1, .channel_mask = 0, .sample_rate = 22050, .mclk_multiple = 0 };
        s_spk_vol = 60;
        esp_codec_dev_open(s_spk, &s_spk_fs);
        esp_codec_dev_set_out_vol(s_spk, s_spk_vol);
        /* Closing the codec must actually power the ES8311 down (not just
         * stop I2S) — otherwise it stays at ~8 mA in standby. */
        esp_codec_set_disable_when_closed(s_spk, true);
        ESP_LOGI(TAG, "Speaker (ES8311) ready");
    }

    s_mic = bsp_audio_codec_microphone_init();
    if (s_mic) {
        s_mic_fs = (esp_codec_dev_sample_info_t){ .bits_per_sample = 16, .channel = 1, .channel_mask = 0, .sample_rate = 16000, .mclk_multiple = 0 };
        esp_codec_dev_open(s_mic, &s_mic_fs);
        esp_codec_set_disable_when_closed(s_mic, true);
        ESP_LOGI(TAG, "Mic (ES7210) ready");
    }

    s_ok = true;
    return ESP_OK;
}

esp_err_t hw_audio_deinit(void)
{
    if (s_spk) { esp_codec_dev_close(s_spk); esp_codec_dev_delete(s_spk); s_spk = NULL; }
    if (s_mic) { esp_codec_dev_close(s_mic); esp_codec_dev_delete(s_mic); s_mic = NULL; }
    s_ok = false;
    return ESP_OK;
}

esp_err_t hw_audio_set_volume(uint8_t pct)
{
    if (!s_spk) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_set_out_vol(s_spk, (int)pct);
}

esp_err_t hw_audio_mute(bool m)
{
    if (!s_spk) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_set_out_mute(s_spk, m);
}

esp_err_t hw_audio_sleep(void)
{
    ESP_LOGI(TAG, "Audio → power down (ES8311 + ES7210)");
    /* Closing powers the codecs down (disable_when_closed) and stops I2S +
     * drops the PA (GPIO46).  Housekeeping / tinyML-reject re-sleeps never
     * call hw_audio_wake(), so the codecs stay off until a real display wake. */
    if (s_mic) esp_codec_dev_close(s_mic);
    if (s_spk) esp_codec_dev_close(s_spk);
    return ESP_OK;
}

esp_err_t hw_audio_wake(void)
{
    ESP_LOGI(TAG, "Audio → wake (reopen codecs)");
    if (s_spk) {
        esp_codec_dev_open(s_spk, &s_spk_fs);
        esp_codec_dev_set_out_vol(s_spk, s_spk_vol);
    }
    if (s_mic) {
        esp_codec_dev_open(s_mic, &s_mic_fs);
    }
    return ESP_OK;
}

esp_codec_dev_handle_t hw_audio_get_mic(void)
{
    return s_mic;
}
