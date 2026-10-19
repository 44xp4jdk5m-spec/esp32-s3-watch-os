/**
 * @file wrist_flick.cpp
 * @brief Wrist-flick "back" gesture detector using the official ESP-DL
 *        component (mirrors the wake-on-raise implementation).
 *
 * The model is a compact 2D-CNN quantised to int8 with esp-ppq and exported
 * as a `.espdl` file (embedded in .rodata by CMake when present).  The
 * detector task samples the QMI8658 at 50 Hz and classifies the last 1.5 s
 * window.  On a confident, debounced "flick" it plays a haptic click and
 * raises g_pending_reset, which the main loop dispatches as a back action
 * (only from sub-screens — the watch face is excluded).
 *
 * Memory: the model is loaded into PSRAM only while the gesture can
 * actually fire (feature enabled + display fully on + sub-screen active),
 * and freed otherwise — so it never competes with wake-on-raise (display
 * off) or the pinball game.
 */

#include "wrist_flick.h"

#include "app_config.h"
#include "app_events.h"
#include "gesture_fusion.h"
#include "hw_imu.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dl_define.hpp"
#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"

static const char *TAG = "WRIST_FLICK";

/* ── Model symbol (embedded by CMake only if flick.espdl exists) ──── */
#ifdef WRIST_FLICK_MODEL_PRESENT
extern const uint8_t flick_espdl[] asm("_binary_flick_espdl_start");
#endif

/* ── Tuning constants (MUST mirror the training pipeline) ─────────── */
#define FLICK_ACCEL_DIV      32.0f    /* m/s^2  -> /32  -> ~[-1,1] */
#define FLICK_GYRO_DIV       512.0f   /* deg/s  -> /512 -> ~[-1,1] */
#define FLICK_SAMPLE_RATE_HZ 50       /* IMU sample rate (matches training) */
#define FLICK_WINDOW         75       /* 1.5 s @ 50 Hz */
#define FLICK_INFER_EVERY    5        /* run inference every 5 samples (100 ms) */
/* Runtime confidence threshold: g_config.wrist_flick_confidence_pct (%), 20-99,
 * default 96.  Retrained model: flicks saturate at ~0.99, not-flick <0.06, so
 * the default is strict yet effective; the slider trades recall vs false
 * positives live — higher = stricter. */
#define FLICK_CONSECUTIVE    2        /* require N consecutive positives */
#define FLICK_COOLDOWN_MS    2000     /* ignore further flicks for 2 s after trigger */
#define FLICK_TASK_STACK     8192
#define FLICK_TASK_PRIO      6

/* Physical gate — the model over-fired on non-flick motion in the field.
 * A real wrist flick is a SHARP, HIGH-VELOCITY snap.  From the training data
 * (90 flicks / 20k not-flick samples): flick windows peak ~1050 deg/s gyro
 * with ~1350 deg/s/sample rise, while not-flick windows are ~90 deg/s
 * (max 482) and ~50 rise (max 261).  Only windows that contain this signature
 * reach the model, which kills scroll / arm-swing / tap false positives.
 * (Normalised by /512, same as the ring buffer.) */
#define FLICK_GYRO_GATE_NORM   (450.0f / FLICK_GYRO_DIV)  /* peak gyro mag */
#define FLICK_SHARP_GATE_NORM  (160.0f / FLICK_GYRO_DIV)  /* peak gyro rise */

/* ── Module state ─────────────────────────────────────────────────── */
static TaskHandle_t      s_task      = NULL;
static SemaphoreHandle_t s_stopped   = NULL;
static volatile bool     s_stop      = false;
static volatile bool     s_running   = false;

#ifdef WRIST_FLICK_MODEL_PRESENT
static dl::Model        *s_model     = nullptr;
static void detection_task(void *arg);
#endif

/* ── Helpers ──────────────────────────────────────────────────────── */

#ifdef WRIST_FLICK_MODEL_PRESENT
static bool flick_screen_allows(void)
{
    switch (g_current_screen) {
    case APP_SCREEN_HONEYCOMB:
    case APP_SCREEN_SETTINGS:
    case APP_SCREEN_PHOTOS:
    case APP_SCREEN_CALCULATOR:
    case APP_SCREEN_NOISE:
    case APP_SCREEN_WATCH_FACE_PICKER:
    case APP_SCREEN_ICON_REARRANGE:
        return true;
    default:
        return false;   /* watch face, charging overlay, boot */
    }
}

/* Flick can fire only while the display is fully on and we are on a
 * sub-screen (never on the watch face, dimmed, AOD or display off).
 *
 * It is NOT gated on the motion pointer: a real flick is a sharp ~1000 deg/s
 * gyro snap while pointer tilting stays slow, so the physical gate below
 * (window_is_flick_like) already keeps the two apart.  Both features can
 * stay enabled together and the wrist can still trigger the back gesture.
 *
 * When the unified gesture_fusion detector is active (both features on +
 * sub-screen + gesture.espdl embedded), THIS detector pauses — the fused
 * module owns gesture recognition then. */
static bool flick_can_fire(void)
{
    return !gesture_fusion_is_active() &&
           !g_display_off && !g_display_dimmed && !g_aod_active &&
           flick_screen_allows();
}

/* Keep the IMU ring warm while the display is up and the feature is on
 * (even on the watch face / dimmed), so a flick right after entering a
 * sub-screen is captured immediately.  Paused while the fused detector
 * samples instead. */
static bool flick_wants_sampling(void)
{
    return !gesture_fusion_is_active() && !g_display_off && !g_aod_active;
}

/* True if the 1.5 s window contains the sharp, high-velocity gyro signature
 * of a real wrist flick: peak gyro magnitude >= gate AND peak per-sample
 * gyro rise >= gate.  Computed from the already-normalised ring buffer. */
static bool window_is_flick_like(const float (*ring)[6], int head, int window)
{
    float peak_mag = 0.0f, peak_rise = 0.0f;
    int prev = (head + window - 1) % window;   /* newest sample (ring wraps) */
    int idx  = head;
    for (int i = 0; i < window; i++) {
        float gx = ring[idx][3], gy = ring[idx][4], gz = ring[idx][5];
        float mag = sqrtf(gx * gx + gy * gy + gz * gz);
        if (mag > peak_mag) peak_mag = mag;

        float pgx = ring[prev][3], pgy = ring[prev][4], pgz = ring[prev][5];
        float rise = sqrtf((gx - pgx) * (gx - pgx) +
                           (gy - pgy) * (gy - pgy) +
                           (gz - pgz) * (gz - pgz));
        if (rise > peak_rise) peak_rise = rise;

        prev = idx;
        idx  = (idx + 1) % window;
    }
    return (peak_mag >= FLICK_GYRO_GATE_NORM) &&
           (peak_rise >= FLICK_SHARP_GATE_NORM);
}
#endif /* WRIST_FLICK_MODEL_PRESENT */

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t wrist_flick_init(void)
{
    if (!s_stopped) {
        s_stopped = xSemaphoreCreateBinary();
    }
    return s_stopped ? ESP_OK : ESP_ERR_NO_MEM;
}

bool wrist_flick_is_enabled(void)
{
    return g_config.wrist_flick_enabled;
}

bool wrist_flick_is_running(void)
{
    return s_running;
}

esp_err_t wrist_flick_start(void)
{
    if (s_task) {
        ESP_LOGW(TAG, "detector already running");
        return ESP_OK;
    }
#ifndef WRIST_FLICK_MODEL_PRESENT
    ESP_LOGW(TAG, "flick.espdl model not embedded — train it first "
                  "(tinyml_mpu/espdl_model/train_flick.py)");
    return ESP_ERR_NOT_FOUND;
#else
    s_stop = false;
    BaseType_t ok = xTaskCreatePinnedToCore(detection_task, "wrist_flick",
                                            FLICK_TASK_STACK, NULL,
                                            FLICK_TASK_PRIO, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create detector task");
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    ESP_LOGI(TAG, "Detector started (back gesture)");
    return ESP_OK;
#endif
}

void wrist_flick_stop(void)
{
    if (!s_task) {
        s_stop = true;
        return;
    }
    if (s_task == xTaskGetCurrentTaskHandle()) {
        s_stop = true;
        return;
    }
    s_stop = true;
    if (s_stopped && xSemaphoreTake(s_stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "timed out waiting for detector to stop");
        return;
    }
    s_task    = NULL;
    s_running = false;
}

/* ── Detection task ───────────────────────────────────────────────── */

#ifdef WRIST_FLICK_MODEL_PRESENT
static void detection_task(void *arg)
{
    (void)arg;

    /* Ring buffer for the last 1.5 s of normalised IMU data (in PSRAM). */
    float (*ring)[6] = (float (*)[6])heap_caps_malloc(
        FLICK_WINDOW * 6 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) {
        ESP_LOGE(TAG, "ring buffer alloc failed");
        if (s_stopped) xSemaphoreGive(s_stopped);
        s_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int  head   = 0;
    bool full   = false;
    int  infer  = 0;
    int  consec = 0;
    int64_t cooldown_until = 0;

    TickType_t last_tick = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / FLICK_SAMPLE_RATE_HZ);

    while (!s_stop) {
        bool enabled = g_config.wrist_flick_enabled;

        /* Keep sampling while the display is up and the feature is on. */
        if (enabled && flick_wants_sampling()) {
            hw_imu_data_t d;
            if (hw_imu_read(&d) == ESP_OK) {
                ring[head][0] = d.accel_x / FLICK_ACCEL_DIV;
                ring[head][1] = d.accel_y / FLICK_ACCEL_DIV;
                ring[head][2] = d.accel_z / FLICK_ACCEL_DIV;
                ring[head][3] = d.gyro_x  / FLICK_GYRO_DIV;
                ring[head][4] = d.gyro_y  / FLICK_GYRO_DIV;
                ring[head][5] = d.gyro_z  / FLICK_GYRO_DIV;
                head = (head + 1) % FLICK_WINDOW;
                if (head == 0) full = true;
            }
        }

        bool can_fire = enabled && flick_can_fire();

        /* Load the model on demand (keeps PSRAM free when inactive). */
        if (can_fire && !s_model) {
#ifdef WRIST_FLICK_MODEL_PRESENT
            dl::Model *m = new (std::nothrow) dl::Model(
                (const char *)flick_espdl,
                fbs::MODEL_LOCATION_IN_FLASH_RODATA,
                0,                          /* all activations in PSRAM */
                dl::MEMORY_MANAGER_GREEDY,
                nullptr,
                false);                     /* weights stay in FLASH */
            if (!m) {
                ESP_LOGE(TAG, "failed to allocate ESP-DL model");
            } else {
                s_model = m;
                ESP_LOGI(TAG, "model loaded (PSRAM free: %" PRIu32 " B)",
                         (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            }
#else
            /* Model not embedded — nothing to do. */
#endif
        } else if (!can_fire && s_model) {
            dl::Model *m = s_model;
            s_model = nullptr;
            delete m;
            ESP_LOGI(TAG, "model unloaded (back gesture inactive)");
        }

        if (can_fire && s_model && full) {
            infer++;
            if (infer >= FLICK_INFER_EVERY) {
                infer = 0;
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms < cooldown_until) continue;   /* in cooldown */

                /* Physical gate: only classify windows that actually contain
                 * a sharp, high-velocity flick signature.  This skips the CNN
                 * for scrolls / taps / arm-swing and prevents false backs. */
                if (!window_is_flick_like(ring, head, FLICK_WINDOW)) {
                    consec = 0;
                    continue;
                }

                dl::TensorBase *input  = s_model->get_input();
                dl::TensorBase *output = s_model->get_output();
                int8_t *in_data        = input->get_element_ptr<int8_t>();
                float   in_inv_scale   = DL_RESCALE(input->exponent);
                float   out_scale      = DL_SCALE(output->exponent);

                /* Fill the int8 input with the 75 most recent samples. */
                int idx = head;
                for (int i = 0; i < FLICK_WINDOW; i++) {
                    in_data[i * 6 + 0] = dl::quantize<int8_t>(ring[idx][0], in_inv_scale);
                    in_data[i * 6 + 1] = dl::quantize<int8_t>(ring[idx][1], in_inv_scale);
                    in_data[i * 6 + 2] = dl::quantize<int8_t>(ring[idx][2], in_inv_scale);
                    in_data[i * 6 + 3] = dl::quantize<int8_t>(ring[idx][3], in_inv_scale);
                    in_data[i * 6 + 4] = dl::quantize<int8_t>(ring[idx][4], in_inv_scale);
                    in_data[i * 6 + 5] = dl::quantize<int8_t>(ring[idx][5], in_inv_scale);
                    idx = (idx + 1) % FLICK_WINDOW;
                }

                s_model->run(input, dl::RUNTIME_MODE_AUTO);

                int8_t q    = output->get_element_ptr<int8_t>()[0];
                float  prob = dl::dequantize<int8_t, float>(q, out_scale);
                float  conf = (float)g_config.wrist_flick_confidence_pct / 100.0f;
                if (conf < 0.2f)  conf = 0.2f;
                if (conf > 0.99f) conf = 0.99f;
                if (prob >= conf) {
                    if (++consec >= FLICK_CONSECUTIVE) {
                        ESP_LOGI(TAG, ">>> FLICK DETECTED (%.3f) — back <<<", prob);
                        cooldown_until = esp_timer_get_time() / 1000 + FLICK_COOLDOWN_MS;
                        consec = 0;
                        app_events_request_vibe_click();
                        g_pending_reset = true;   /* main loop = back */
                    }
                } else {
                    consec = 0;
                }
            }
        } else {
            consec = 0;
        }

        /* 20 ms sampling tick; wake the loop sooner if stopped. */
        vTaskDelayUntil(&last_tick, period);
    }

    heap_caps_free(ring);

    if (s_model) {
        dl::Model *m = s_model;
        s_model = nullptr;
        delete m;
    }

    s_running = false;
    if (s_stopped) xSemaphoreGive(s_stopped);
    s_task = NULL;
    vTaskDelete(NULL);
}
#endif /* WRIST_FLICK_MODEL_PRESENT */
