/**
 * @file wake_on_raise.cpp
 * @brief Wake-on-raise implementation using the official ESP-DL component.
 *
 * The model is a compact 2D-CNN quantised to int8 with esp-ppq and exported
 * as a `.espdl` file (embedded in .rodata).  It is loaded into PSRAM only
 * while the display is off:
 *   - max_internal_size = 0  -> every activation tensor lives in PSRAM
 *   - param_copy = false     -> weights stay in FLASH (saves PSRAM)
 * The detector task samples the QMI8658 at 50 Hz, feeds the last 1.5 s window
 * into the model every 100 ms and wakes the display on a confident, debounced
 * "raise" classification.
 */

#include "wake_on_raise.h"

#include "app_config.h"
#include "app_power.h"
#include "hw_imu.h"
#include "hw_sleep.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ULP-RISC-V sampler */
#include "ulp_riscv.h"
#include "ulp_wor.h"   /* generated: ulp_wor_ring, ulp_wor_idx, ulp_wor_full, ... */

/* Pure-integer 3-layer CNN — the SAME one that runs inside the ULP.  In ULP
 * mode this replaces the (much bigger) ESP-DL model, so a raise is decided
 * entirely on the ULP and the CPU only wakes for a confirmed raise. */
#include "wor_cnn.h"

#include <math.h>

#include "dl_define.hpp"
#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"

static const char *TAG = "WAKE_RAISE";

/* ─────────────────────────────────────────────────────────────────────
 * Model symbols embedded in .rodata by target_add_aligned_binary_data() /
 * ulp_embed_binary().  For "wake_on_raise.espdl" the linker symbol is
 * _binary_wake_on_raise_espdl_start; for the ULP they are
 * _binary_ulp_wor_bin_start/_end.
 * ───────────────────────────────────────────────────────────────────── */
extern const uint8_t wake_on_raise_espdl[] asm("_binary_wake_on_raise_espdl_start");
extern const uint8_t ulp_wor_bin_start[] asm("_binary_ulp_wor_bin_start");
extern const uint8_t ulp_wor_bin_end[]   asm("_binary_ulp_wor_bin_end");

/* ── Tuning constants (MUST mirror the training pipeline) ──────────── */
#define WOR_ACCEL_DIV      32.0f    /* m/s^2  -> /32  -> ~[-1,1] */
#define WOR_GYRO_DIV       512.0f   /* deg/s  -> /512 -> ~[-1,1] */
#define WOR_SAMPLE_RATE_HZ 50       /* IMU sample rate (matches training) */
#define WOR_WINDOW         75       /* 1.5 s @ 50 Hz */
#define WOR_INFER_EVERY    5        /* run inference every 5 samples (100 ms) */
#define WOR_CONFIDENCE     0.98f    /* sigmoid threshold (0.99 rejected real raises) */
#define WOR_CONSECUTIVE    2        /* require N consecutive positives (debounce) */
#define WOR_TASK_STACK     8192
#define WOR_TASK_PRIO      6

/* ULP raw int16 counts -> normalised CNN input:
 *   accel 8G : m/s^2 = raw * 9.807 / 4096 ; then /WOR_ACCEL_DIV
 *   gyro 1024: deg/s = raw / 32          ; then /WOR_GYRO_DIV          */
#define WOR_ACCEL_RAW_NORM (9.807f / (4096.0f * WOR_ACCEL_DIV))
#define WOR_GYRO_RAW_NORM  (1.0f  / (32.0f   * WOR_GYRO_DIV))

/* ── Module state ──────────────────────────────────────────────────── */
static dl::Model        *s_model      = nullptr;
static TaskHandle_t      s_task       = NULL;
static SemaphoreHandle_t s_stopped    = NULL;
static volatile bool     s_stop       = false;
static volatile bool     s_running    = false;

/* ULP sampler mode */
static bool              s_ulp_armed   = false;
static bool              s_ulp_running = false;

static void detection_task(void *arg);

/* ── Model helpers (shared by CPU + ULP modes) ─────────────────────── */

static dl::Model *wor_load_model(void)
{
    /* Load the quantised ESP-DL model (activations in PSRAM, weights in FLASH). */
    dl::Model *m = new (std::nothrow) dl::Model(
        (const char *)wake_on_raise_espdl,
        fbs::MODEL_LOCATION_IN_FLASH_RODATA,
        0,                           /* max_internal_size = 0 -> all in PSRAM */
        dl::MEMORY_MANAGER_GREEDY,
        nullptr,                     /* no encryption key */
        false);                      /* param_copy = false -> weights stay in FLASH */
    if (!m) {
        ESP_LOGE(TAG, "failed to allocate ESP-DL model");
        return nullptr;
    }
    /* One-time self-check: verifies the .espdl was exported with test values
     * and that on-device inference matches the PC reference. Not fatal. */
    if (m->test() == ESP_OK) {
        ESP_LOGI(TAG, "ESP-DL model self-test passed");
    } else {
        ESP_LOGW(TAG, "ESP-DL model self-test failed (test values absent?)");
    }
    return m;
}

static void wor_free_model(void)
{
    if (s_model) {
        delete s_model;
        s_model = nullptr;
    }
}

/* Run the CNN on a normalised [WOR_WINDOW][6] window; returns the prob. */
static float wor_infer_window(dl::Model *m, const float (*window)[6])
{
    dl::TensorBase *input  = m->get_input();
    dl::TensorBase *output = m->get_output();
    int8_t *in_data        = input->get_element_ptr<int8_t>();
    float   in_inv_scale   = DL_RESCALE(input->exponent); /* 1 / input scale */
    float   out_scale      = DL_SCALE(output->exponent);

    for (int i = 0; i < WOR_WINDOW; i++) {
        for (int c = 0; c < 6; c++) {
            in_data[i * 6 + c] = dl::quantize<int8_t>(window[i][c], in_inv_scale);
        }
    }
    m->run(input, dl::RUNTIME_MODE_AUTO);
    int8_t q    = output->get_element_ptr<int8_t>()[0];
    return dl::dequantize<int8_t, float>(q, out_scale);
}

/* ── Public API ────────────────────────────────────────────────────── */

esp_err_t wake_on_raise_init(void)
{
    if (!s_stopped) {
        s_stopped = xSemaphoreCreateBinary();
    }
    return s_stopped ? ESP_OK : ESP_ERR_NO_MEM;
}

bool wake_on_raise_is_enabled(void)
{
    return g_config.wake_on_raise_enabled;
}

bool wake_on_raise_is_running(void)
{
    return s_running;
}

bool wake_on_raise_ulp_is_armed(void)
{
    return s_ulp_armed;
}

bool wake_on_raise_ulp_is_running(void)
{
    return s_ulp_running;
}

esp_err_t wake_on_raise_start(void)
{
    if (s_task) {
        ESP_LOGW(TAG, "detector already running");
        return ESP_OK;
    }
    if (!g_config.wake_on_raise_enabled) {
        ESP_LOGD(TAG, "disabled in config — not starting");
        return ESP_ERR_INVALID_STATE;
    }

    dl::Model *m = wor_load_model();
    if (!m) return ESP_ERR_NO_MEM;
    s_model = m;
    s_stop  = false;

    BaseType_t ok = xTaskCreatePinnedToCore(detection_task, "wake_raise",
                                            WOR_TASK_STACK, NULL,
                                            WOR_TASK_PRIO, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create detector task");
        wor_free_model();
        return ESP_ERR_NO_MEM;
    }
    s_running = true;

    ESP_LOGI(TAG, "CPU detector started (PSRAM free: %" PRIu32 " B)",
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

void wake_on_raise_stop(void)
{
    /* ULP sampler mode active?  Stop that instead of the task. */
    if (s_ulp_armed || s_ulp_running) {
        wake_on_raise_ulp_stop();
        return;
    }

    if (!s_task) {
        s_stop = true;
        return;
    }

    if (s_task == xTaskGetCurrentTaskHandle()) {
        /* Called from the detector itself (raise detected): it will clean up. */
        s_stop = true;
        return;
    }

    /* Another task: request stop and wait for the detector to free the model. */
    s_stop = true;
    if (s_stopped && xSemaphoreTake(s_stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "timed out waiting for detector to stop");
        /* Leave s_task set: wake_on_raise_start() will refuse until the
         * detector exits on its own and clears the handle. */
        return;
    }
    s_task    = NULL;
    s_running = false;
}

/* ── ULP-RISC-V sampler mode ───────────────────────────────────────── */

esp_err_t wake_on_raise_ulp_prepare(void)
{
    if (s_ulp_armed) return ESP_OK;

    /* The ULP sampler runs during ANY light sleep: it samples the IMU and
     * does the raise gate.  Touch wake is via GPIO38 (the FT3168 INT), not
     * the ULP. */
    bool wor = g_config.wake_on_raise_enabled;

    /* The raise CNN runs INSIDE the ULP (wor_cnn_score, pure integer), so no
     * ESP-DL model is loaded for the ULP path — the CPU is not woken for
     * every gate fire, only for a raise the ULP CNN has already confirmed. */
    hw_sleep_set_keep_imu(wor);
    hw_sleep_set_ulp_wakeup(true);

    s_ulp_armed = true;
    ESP_LOGI(TAG, "ULP sleep sampler armed (IMU raise gate %s, CNN in ULP)",
             wor ? "on" : "off");
    return ESP_OK;
}

esp_err_t wake_on_raise_ulp_run(void)
{
    if (!s_ulp_armed) return ESP_ERR_INVALID_STATE;
    if (s_ulp_running) return ESP_OK;

    esp_err_t ret = ulp_riscv_load_binary(ulp_wor_bin_start,
                                          (size_t)(ulp_wor_bin_end - ulp_wor_bin_start));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ULP load failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ulp_riscv_run();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ULP run failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Tell the ULP whether to also do the IMU raise gate.  Must be set AFTER
     * ulp_riscv_load_binary (which zeroes the whole reserved RTC region). */
    ulp_wor_imu_enabled = g_config.wake_on_raise_enabled ? 1 : 0;

    s_ulp_running = true;
    ESP_LOGI(TAG, "ULP sampler running (bit-banged IMU + touch @ ~50Hz)");
    return ESP_OK;
}

void wake_on_raise_ulp_halt(void)
{
    if (!s_ulp_running) return;

    /* Stop the ULP first (it drives GPIO14/15 for bit-banged I2C). */
    ulp_riscv_timer_stop();
    ulp_riscv_halt();
    s_ulp_running = false;

    /* Release the I2C bus: if the ULP was halted mid-transaction a slave
     * (FT3168/QMI8658/PMIC) may still be holding SDA low, which would wedge
     * every main-CPU I2C transaction after wake.  Drive both lines high and
     * clock SCL (standard I2C bus recovery) before handing the pins back. */
    rtc_gpio_set_direction(GPIO_NUM_14, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_direction(GPIO_NUM_15, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_NUM_15, 1);   /* SDA released high */
    for (int i = 0; i < 10; i++) {
        rtc_gpio_set_level(GPIO_NUM_14, 0);   /* SCL low */
        esp_rom_delay_us(5);
        rtc_gpio_set_level(GPIO_NUM_14, 1);   /* SCL high */
        esp_rom_delay_us(5);
    }

    /* Hand GPIO14/15 back to the digital IO mux (I2C function). */
    rtc_gpio_deinit(GPIO_NUM_14);
    rtc_gpio_deinit(GPIO_NUM_15);
    ESP_LOGI(TAG, "ULP sampler halted, I2C bus released + pins restored");
}

esp_err_t wake_on_raise_ulp_handle_wake(bool *raise, float *prob_out)
{
    *raise = false;
    if (prob_out) *prob_out = 0.0f;
    if (!s_ulp_armed) return ESP_ERR_INVALID_STATE;

    /* IMU raise-gate wake (touch wakes arrive via GPIO38 INT, not the ULP). */
    if (!ulp_wor_full) {
        ESP_LOGW(TAG, "ULP gate wake before ring full — ignoring");
        return ESP_ERR_INVALID_STATE;
    }

    /* The ULP already classified this window with the same integer CNN; we
     * re-run it here (~1 ms on the CPU) for the final decision + diagnostics.
     * The ULP is halted, so the ring is stable.  No ESP-DL model involved. */
    const int16_t *ring = (const int16_t *)(const void *)ulp_wor_ring;
    int32_t logit = wor_cnn_score(ring, (uint32_t)ulp_wor_idx);
    float prob = 1.0f / (1.0f + expf(-(float)logit / 32768.0f));

    ESP_LOGI(TAG, "ULP gate wake — CNN logit=%" PRId32 " prob=%.3f "
                 "(thr %" PRId32 ", imu_samples=%" PRIu32 ")",
             logit, (double)prob, (int32_t)WOR_CNN_LOGIT_TH,
             (uint32_t)ulp_wor_sample_count);
    if (prob_out) *prob_out = prob;
    *raise = (logit >= WOR_CNN_LOGIT_TH);
    return ESP_OK;
}

uint32_t wake_on_raise_ulp_sample_count(void)
{
    if (!s_ulp_armed) return 0;
    return (uint32_t)ulp_wor_sample_count;
}

void wake_on_raise_ulp_stop(void)
{
    wake_on_raise_ulp_halt();
    s_ulp_armed = false;
    hw_sleep_set_keep_imu(false);
    hw_sleep_set_ulp_wakeup(false);
    wor_free_model();
    ESP_LOGI(TAG, "ULP wake-on-raise stopped");
}

/* ── Detection task (CPU mode) ─────────────────────────────────────── */

static void detection_task(void *arg)
{
    (void)arg;

    /* Ring buffer for the last 1.5 s of normalised IMU data (in PSRAM). */
    float (*ring)[6] = (float (*)[6])heap_caps_malloc(
        WOR_WINDOW * 6 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring) {
        dl::TensorBase *input = s_model->get_input();
        ESP_LOGI(TAG, "input [%d,%d,%d,%d] exp=%d  output exp=%d",
                 input->shape[0], input->shape[1], input->shape[2], input->shape[3],
                 (int)input->exponent, (int)s_model->get_output()->exponent);

        int  head   = 0;
        bool full   = false;
        int  infer  = 0;
        int  consec = 0;

        TickType_t last_tick = xTaskGetTickCount();
        const TickType_t period = pdMS_TO_TICKS(1000 / WOR_SAMPLE_RATE_HZ);

        while (!s_stop) {
            hw_imu_data_t d;
            if (hw_imu_read(&d) == ESP_OK) {
                ring[head][0] = d.accel_x / WOR_ACCEL_DIV;
                ring[head][1] = d.accel_y / WOR_ACCEL_DIV;
                ring[head][2] = d.accel_z / WOR_ACCEL_DIV;
                ring[head][3] = d.gyro_x  / WOR_GYRO_DIV;
                ring[head][4] = d.gyro_y  / WOR_GYRO_DIV;
                ring[head][5] = d.gyro_z  / WOR_GYRO_DIV;
                head = (head + 1) % WOR_WINDOW;
                if (head == 0) full = true;
            }

            infer++;
            if (full && infer >= WOR_INFER_EVERY) {
                infer = 0;

                /* Run the CNN on the 75 most recent samples, oldest first. */
                float window[WOR_WINDOW][6];
                int idx = head;
                for (int i = 0; i < WOR_WINDOW; i++) {
                    memcpy(window[i], ring[idx], 6 * sizeof(float));
                    idx = (idx + 1) % WOR_WINDOW;
                }
                float prob = wor_infer_window(s_model, window);

                if (prob >= WOR_CONFIDENCE) {
                    if (++consec >= WOR_CONSECUTIVE) {
                        ESP_LOGI(TAG, ">>> RAISE DETECTED (%.3f) — waking <<<", prob);
                        s_stop = true;
                        app_power_wake();   /* turns display on; stops this detector */
                        break;
                    }
                } else {
                    consec = 0;
                }
            }

            vTaskDelayUntil(&last_tick, period);
        }

        heap_caps_free(ring);
    } else {
        ESP_LOGE(TAG, "ring buffer alloc failed");
    }

    /* Free the model & signal completion to any task blocked in stop(). */
    dl::Model *m = s_model;
    s_model = nullptr;
    delete m;

    s_running = false;
    if (s_stopped) xSemaphoreGive(s_stopped);
    s_task = NULL;
    vTaskDelete(NULL);
}
