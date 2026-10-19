/**
 * @file double_tap.cpp
 * @brief Double-tap "click" gesture detector using the official ESP-DL
 *        component (mirrors the wrist-flick implementation).
 *
 * The model is the same compact 2D-CNN architecture as the flick model,
 * quantised to int8 with esp-ppq and exported as a `.espdl` file (embedded in
 * .rodata by CMake when present).  The detector task samples the QMI8658 at
 * 50 Hz and classifies the last 1.5 s window.  On a confident, debounced
 * "double tap" (index finger + thumb tapped together twice) it asks the air
 * pointer to click at the cursor position.
 *
 * A double tap has a distinct PHYSICAL signature: two sharp |accel| impulses
 * (|a| - g > ~3 m/s^2) in quick succession.  Before running the CNN we require
 * exactly that — at least two impulses in the window — which cleanly rejects
 * pure tilt (moving the cursor) and scroll, where |a| stays at ~g.
 *
 * Memory: the model is loaded into PSRAM only while the gesture can actually
 * fire (air pointer active + display fully on) and freed otherwise, so it
 * never competes with the display-off wake-on-raise detector.
 */

#include "double_tap.h"

#include "air_pointer.h"
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

static const char *TAG = "DOUBLE_TAP";

/* ── Model symbol (embedded by CMake only if tap.espdl exists) ────── */
#ifdef DOUBLE_TAP_MODEL_PRESENT
extern const uint8_t tap_espdl[] asm("_binary_tap_espdl_start");
#endif

/* ── Tuning constants (MUST mirror the training pipeline) ─────────── */
#define TAP_ACCEL_DIV      32.0f    /* m/s^2  -> /32  -> ~[-1,1] */
#define TAP_GYRO_DIV       512.0f   /* deg/s  -> /512 -> ~[-1,1] */
#define TAP_SAMPLE_RATE_HZ 50       /* IMU sample rate (matches training) */
#define TAP_WINDOW         75       /* 1.5 s @ 50 Hz */
#define TAP_INFER_EVERY    5        /* run inference every 5 samples (100 ms) */
/* Trained model: real double-taps saturate near 1.0, not-flick <0.1, so a
 * strict default keeps tilt/scroll from clicking.  (deploy_info.txt said the
 * tuned quantised threshold is 0.0938.) */
#define TAP_CONFIDENCE     0.90f
#define TAP_CONSECUTIVE    2        /* require N consecutive positives */
#define TAP_COOLDOWN_MS    1500     /* ignore further taps for 1.5 s after click */
#define TAP_TASK_STACK     8192
#define TAP_TASK_PRIO      6
/* Click gate: the double-tap may only click while the watch is within +-35 deg
 * of flat (the pointer deadzone + slow zone).  Beyond 35 deg the pointer is
 * moving fast, so a user has no reason to click — and the gesture is ignored. */
#define TAP_TILT_MAX_DEG   35.0f

/* Physical gate — a real double tap produces at least two SHARP accelerometer
 * impulses (|a|-g > 3 m/s^2).  Pure tilt / scroll keeps |a| ~ g, so this skips
 * the CNN for cursor movement.  Same segmentation as train_tap.py. */
#define TAP_DYN_GATE_MS2   3.0f     /* |a|-g impulse threshold (m/s^2) */
#define TAP_IMPULSE_GAP    8        /* refractory gap between impulses (samples) */
#define TAP_MIN_IMPULSES   2        /* need >= 2 impulses in the window */

/* ── Module state ─────────────────────────────────────────────────── */
static TaskHandle_t      s_task      = NULL;
static SemaphoreHandle_t s_stopped   = NULL;
static volatile bool     s_stop      = false;
static volatile bool     s_running   = false;

/* Latest tilt of the watch (deg, 0 = flat) — updated from each IMU sample
 * and used to gate clicking to within +-35 deg. */
static float             s_tilt_roll_deg;
static float             s_tilt_pitch_deg;

#ifdef DOUBLE_TAP_MODEL_PRESENT
static dl::Model        *s_model     = nullptr;
static void detection_task(void *arg);
#endif

/* ── Helpers ──────────────────────────────────────────────────────── */

#ifdef DOUBLE_TAP_MODEL_PRESENT
/* The double tap is the click mechanism of the motion pointer, so it only
 * makes sense while the pointer is active.  Requires the display fully on
 * (never dimmed / AOD / off) AND the watch within +-35 deg of flat (while
 * moving fast there is no reason to click). */
static bool tap_can_fire(void)
{
    return air_pointer_is_active() &&
           !gesture_fusion_is_active() &&
           !g_display_off && !g_display_dimmed && !g_aod_active &&
           fabsf(s_tilt_roll_deg)  <= TAP_TILT_MAX_DEG &&
           fabsf(s_tilt_pitch_deg) <= TAP_TILT_MAX_DEG;
}

/* Keep the ring warm while the pointer is up (even mid-scroll) so a tap
 * right after movement is captured.  Paused while the fused detector
 * samples instead. */
static bool tap_wants_sampling(void)
{
    return air_pointer_is_active() && !gesture_fusion_is_active() &&
           !g_display_off && !g_aod_active;
}

/* True if the 1.5 s window contains >= 2 distinct accelerometer impulses
 * (|a|-g above the gate).  Computed from the already-normalised ring buffer
 * (accel / TAP_ACCEL_DIV, so multiply back by TAP_ACCEL_DIV). */
static bool window_has_taps(const float (*ring)[6], int head, int window)
{
    int impulses = 0;
    int prev_hit = -1000;
    int idx = head;
    for (int i = 0; i < window; i++) {
        float ax = ring[idx][0] * TAP_ACCEL_DIV;
        float ay = ring[idx][1] * TAP_ACCEL_DIV;
        float az = ring[idx][2] * TAP_ACCEL_DIV;
        float dyn = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 9.81f);
        if (dyn > TAP_DYN_GATE_MS2 && (i - prev_hit) > TAP_IMPULSE_GAP) {
            impulses++;
            prev_hit = i;
        }
        idx = (idx + 1) % window;
    }
    return impulses >= TAP_MIN_IMPULSES;
}
#endif /* DOUBLE_TAP_MODEL_PRESENT */

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t double_tap_init(void)
{
    if (!s_stopped) {
        s_stopped = xSemaphoreCreateBinary();
    }
    return s_stopped ? ESP_OK : ESP_ERR_NO_MEM;
}

bool double_tap_is_active(void)
{
    return s_running;
}

esp_err_t double_tap_start(void)
{
    if (s_task) {
        ESP_LOGW(TAG, "detector already running");
        return ESP_OK;
    }
#ifndef DOUBLE_TAP_MODEL_PRESENT
    ESP_LOGW(TAG, "tap.espdl model not embedded — train it first "
                  "(tinyml_mpu/espdl_model/train_tap.py)");
    return ESP_ERR_NOT_FOUND;
#else
    s_stop = false;
    BaseType_t ok = xTaskCreatePinnedToCore(detection_task, "double_tap",
                                            TAP_TASK_STACK, NULL,
                                            TAP_TASK_PRIO, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create detector task");
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    ESP_LOGI(TAG, "Detector started (double-tap click)");
    return ESP_OK;
#endif
}

void double_tap_stop(void)
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

#ifdef DOUBLE_TAP_MODEL_PRESENT
static void detection_task(void *arg)
{
    (void)arg;

    /* Ring buffer for the last 1.5 s of normalised IMU data (in PSRAM). */
    float (*ring)[6] = (float (*)[6])heap_caps_malloc(
        TAP_WINDOW * 6 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    const TickType_t period = pdMS_TO_TICKS(1000 / TAP_SAMPLE_RATE_HZ);

    while (!s_stop) {
        /* Keep sampling while the air pointer is up. */
        if (tap_wants_sampling()) {
            hw_imu_data_t d;
            if (hw_imu_read(&d) == ESP_OK) {
                ring[head][0] = d.accel_x / TAP_ACCEL_DIV;
                ring[head][1] = d.accel_y / TAP_ACCEL_DIV;
                ring[head][2] = d.accel_z / TAP_ACCEL_DIV;
                ring[head][3] = d.gyro_x  / TAP_GYRO_DIV;
                ring[head][4] = d.gyro_y  / TAP_GYRO_DIV;
                ring[head][5] = d.gyro_z  / TAP_GYRO_DIV;
                head = (head + 1) % TAP_WINDOW;
                if (head == 0) full = true;

                /* Track current tilt (0 = watch flat face-up) for the
                 * +-35 deg click gate — same reference as the air pointer. */
                s_tilt_roll_deg  = atan2f(d.accel_y,
                    sqrtf(d.accel_x * d.accel_x + d.accel_z * d.accel_z)) *
                    (180.0f / 3.14159265f);
                s_tilt_pitch_deg = atan2f(d.accel_x,
                    sqrtf(d.accel_y * d.accel_y + d.accel_z * d.accel_z)) *
                    (180.0f / 3.14159265f);
            }
        }

        bool can_fire = tap_can_fire();

        /* Load the model on demand (keeps PSRAM free when inactive). */
        if (can_fire && !s_model) {
            dl::Model *m = new (std::nothrow) dl::Model(
                (const char *)tap_espdl,
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
        } else if (!can_fire && s_model) {
            dl::Model *m = s_model;
            s_model = nullptr;
            delete m;
            ESP_LOGI(TAG, "model unloaded (double-tap inactive)");
        }

        if (can_fire && s_model && full) {
            infer++;
            if (infer >= TAP_INFER_EVERY) {
                infer = 0;
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms < cooldown_until) continue;   /* in cooldown */

                /* Physical gate: only classify windows that actually contain
                 * the two-impulse double-tap signature (skips cursor tilt). */
                if (!window_has_taps(ring, head, TAP_WINDOW)) {
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
                for (int i = 0; i < TAP_WINDOW; i++) {
                    in_data[i * 6 + 0] = dl::quantize<int8_t>(ring[idx][0], in_inv_scale);
                    in_data[i * 6 + 1] = dl::quantize<int8_t>(ring[idx][1], in_inv_scale);
                    in_data[i * 6 + 2] = dl::quantize<int8_t>(ring[idx][2], in_inv_scale);
                    in_data[i * 6 + 3] = dl::quantize<int8_t>(ring[idx][3], in_inv_scale);
                    in_data[i * 6 + 4] = dl::quantize<int8_t>(ring[idx][4], in_inv_scale);
                    in_data[i * 6 + 5] = dl::quantize<int8_t>(ring[idx][5], in_inv_scale);
                    idx = (idx + 1) % TAP_WINDOW;
                }

                s_model->run(input, dl::RUNTIME_MODE_AUTO);

                int8_t q    = output->get_element_ptr<int8_t>()[0];
                float  prob = dl::dequantize<int8_t, float>(q, out_scale);

                if (prob >= TAP_CONFIDENCE) {
                    if (++consec >= TAP_CONSECUTIVE) {
                        ESP_LOGI(TAG, ">>> DOUBLE-TAP DETECTED (%.3f) — click <<<", prob);
                        cooldown_until = esp_timer_get_time() / 1000 + TAP_COOLDOWN_MS;
                        consec = 0;
                        app_events_request_vibe_click();
                        air_pointer_click();   /* click at the cursor */
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
#endif /* DOUBLE_TAP_MODEL_PRESENT */
