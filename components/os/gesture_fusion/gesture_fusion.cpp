/**
 * @file gesture_fusion.cpp
 * @brief Unified 3-state gesture detector (idle / wrist / double-tap).
 *
 * When the wrist-flick "back" gesture AND the motion pointer are BOTH
 * enabled, running the two separate ESP-DL detectors on the same IMU was
 * wasteful and (previously) mutually exclusive.  This module replaces both
 * with a SINGLE model that outputs one of three states per 1.5 s window:
 *
 *     idle       -> nothing
 *     wrist      -> haptic + g_pending_reset (back, same path as wrist_flick)
 *     double-tap -> haptic + air_pointer_click() (click, same as double_tap)
 *
 * Input format is identical to the flick/tap models (6 channels, 75 samples
 * @ 50 Hz, accel/32 + gyro/512), so the same ring buffer and the same
 * physical gates apply per class:
 *   - "wrist" requires the sharp high-velocity gyro signature (~1000 deg/s)
 *   - "double-tap" requires >= 2 accel impulses while the watch is within
 *     +-35 deg of flat.
 *
 * The model is embedded on demand (PSRAM) only while this detector can
 * actually fire; without gesture.espdl the module is inert and the separate
 * detectors keep working.  While it IS active, wrist_flick and double_tap
 * pause via gesture_fusion_is_active().
 */

#include "gesture_fusion.h"

#include "air_pointer.h"
#include "app_config.h"
#include "app_events.h"
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

static const char *TAG = "GESTURE";

/* ── Model symbol (embedded by CMake only if gesture.espdl exists) ─── */
#ifdef GESTURE_FUSION_MODEL_PRESENT
extern const uint8_t gesture_espdl[] asm("_binary_gesture_espdl_start");
#endif

/* ── Tuning constants (MUST mirror the training pipeline) ─────────── */
#define GES_ACCEL_DIV      32.0f    /* m/s^2  -> /32  -> ~[-1,1] */
#define GES_GYRO_DIV       512.0f   /* deg/s  -> /512 -> ~[-1,1] */
#define GES_SAMPLE_RATE_HZ 50       /* IMU sample rate (matches training) */
#define GES_WINDOW         75       /* 1.5 s @ 50 Hz */
#define GES_INFER_EVERY    5        /* run inference every 5 samples (100 ms) */
#define GES_CONSECUTIVE    2        /* require N consecutive positives */
#define GES_WRIST_COOLDOWN_MS 2000  /* ignore further wrists for 2 s */
#define GES_TAP_COOLDOWN_MS   1500  /* ignore further taps for 1.5 s */
#define GES_TASK_STACK     8192
#define GES_TASK_PRIO      6

/* Model output class indices — MUST match the training order in
 * train_gesture.py (0 = idle, 1 = wrist, 2 = double-tap). */
#define GES_CLASS_IDLE      0
#define GES_CLASS_WRIST     1
#define GES_CLASS_TAP       2

/* Wrist physical gate (same numbers as wrist_flick): real flicks peak
 * ~1050 deg/s gyro with ~1350 deg/s/sample rise; tilting/scroll stays slow. */
#define GES_GYRO_GATE_NORM   (450.0f / GES_GYRO_DIV)  /* peak gyro mag */
#define GES_SHARP_GATE_NORM  (160.0f / GES_GYRO_DIV)  /* peak gyro rise */

/* Double-tap physical gate (same numbers as double_tap): >= 2 accel
 * impulses (|a| - g > 3 m/s^2) and the watch within +-35 deg of flat. */
#define GES_DYN_GATE_MS2    3.0f
#define GES_IMPULSE_GAP     8
#define GES_MIN_IMPULSES    2
#define GES_TILT_MAX_DEG    35.0f

/* Tap confidence (mirrors double_tap.cpp; trained taps saturate ~1.0). */
#define GES_TAP_CONFIDENCE  0.90f

/* ── Module state ─────────────────────────────────────────────────── */
static TaskHandle_t      s_task      = NULL;
static SemaphoreHandle_t s_stopped   = NULL;
static volatile bool     s_stop      = false;
static volatile bool     s_running   = false;

static float             s_roll_deg, s_pitch_deg;   /* latest tilt (for tap gate) */

#ifdef GESTURE_FUSION_MODEL_PRESENT
static dl::Model        *s_model     = nullptr;
static void detection_task(void *arg);
#endif

/* ── Helpers ──────────────────────────────────────────────────────── */

#ifdef GESTURE_FUSION_MODEL_PRESENT
/* The back gesture only applies on sub-screens (same list as wrist_flick). */
static bool fused_screen_allows(void)
{
    switch (g_current_screen) {
    case APP_SCREEN_HONEYCOMB:
    case APP_SCREEN_SETTINGS:
    case APP_SCREEN_PHOTOS:
    case APP_SCREEN_CALCULATOR:
    case APP_SCREEN_NOISE:
    case APP_SCREEN_ACTIVITY:
    case APP_SCREEN_WATCH_FACE_PICKER:
    case APP_SCREEN_ICON_REARRANGE:
        return true;
    default:
        return false;   /* watch face, charging overlay, boot */
    }
}

/* Both features must be enabled and the display fully on. */
static bool fused_can_fire(void)
{
    return g_config.wrist_flick_enabled && g_config.air_pointer_enabled &&
           !g_display_off && !g_display_dimmed && !g_aod_active &&
           fused_screen_allows();
}

/* Keep the IMU ring warm while both features are on and the display is up. */
static bool fused_wants_sampling(void)
{
    return g_config.wrist_flick_enabled && g_config.air_pointer_enabled &&
           !g_display_off && !g_aod_active;
}

/* True if the window contains the sharp gyro signature of a wrist flick. */
static bool window_is_wrist_like(const float (*ring)[6], int head, int window)
{
    float peak_mag = 0.0f, peak_rise = 0.0f;
    int prev = (head + window - 1) % window;
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
    return (peak_mag >= GES_GYRO_GATE_NORM) &&
           (peak_rise >= GES_SHARP_GATE_NORM);
}

/* True if the window contains >= 2 distinct accelerometer impulses. */
static bool window_is_tap_like(const float (*ring)[6], int head, int window)
{
    int impulses = 0;
    int prev_hit = -1000;
    int idx = head;
    for (int i = 0; i < window; i++) {
        float ax = ring[idx][0] * GES_ACCEL_DIV;
        float ay = ring[idx][1] * GES_ACCEL_DIV;
        float az = ring[idx][2] * GES_ACCEL_DIV;
        float dyn = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 9.81f);
        if (dyn > GES_DYN_GATE_MS2 && (i - prev_hit) > GES_IMPULSE_GAP) {
            impulses++;
            prev_hit = i;
        }
        idx = (idx + 1) % window;
    }
    return impulses >= GES_MIN_IMPULSES;
}
#endif /* GESTURE_FUSION_MODEL_PRESENT */

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t gesture_fusion_init(void)
{
    if (!s_stopped) {
        s_stopped = xSemaphoreCreateBinary();
    }
    return s_stopped ? ESP_OK : ESP_ERR_NO_MEM;
}

bool gesture_fusion_is_active(void)
{
#ifdef GESTURE_FUSION_MODEL_PRESENT
    /* The fused detector only "owns" gesture recognition when it can
     * actually fire (both features on, sub-screen, display fully on). */
    return s_running && fused_can_fire();
#else
    return false;   /* no fused model — separate detectors stay in charge */
#endif
}

esp_err_t gesture_fusion_start(void)
{
    if (s_task) {
        ESP_LOGW(TAG, "detector already running");
        return ESP_OK;
    }
#ifndef GESTURE_FUSION_MODEL_PRESENT
    ESP_LOGW(TAG, "gesture.espdl not embedded — train it first "
                  "(tinyml_mpu/espdl_model/train_gesture.py)");
    return ESP_ERR_NOT_FOUND;
#else
    s_stop = false;
    BaseType_t ok = xTaskCreatePinnedToCore(detection_task, "gesture_fuse",
                                            GES_TASK_STACK, NULL,
                                            GES_TASK_PRIO, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create detector task");
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    ESP_LOGI(TAG, "Detector started (unified idle/wrist/double-tap)");
    return ESP_OK;
#endif
}

void gesture_fusion_stop(void)
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

#ifdef GESTURE_FUSION_MODEL_PRESENT
static void detection_task(void *arg)
{
    (void)arg;

    /* Ring buffer for the last 1.5 s of normalised IMU data (in PSRAM). */
    float (*ring)[6] = (float (*)[6])heap_caps_malloc(
        GES_WINDOW * 6 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    int  wrist_consec = 0;
    int  tap_consec   = 0;
    int64_t wrist_cooldown_until = 0;
    int64_t tap_cooldown_until   = 0;

    TickType_t last_tick = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / GES_SAMPLE_RATE_HZ);

    while (!s_stop) {
        /* Sample while both features want the IMU and the display is up. */
        if (fused_wants_sampling()) {
            hw_imu_data_t d;
            if (hw_imu_read(&d) == ESP_OK) {
                ring[head][0] = d.accel_x / GES_ACCEL_DIV;
                ring[head][1] = d.accel_y / GES_ACCEL_DIV;
                ring[head][2] = d.accel_z / GES_ACCEL_DIV;
                ring[head][3] = d.gyro_x  / GES_GYRO_DIV;
                ring[head][4] = d.gyro_y  / GES_GYRO_DIV;
                ring[head][5] = d.gyro_z  / GES_GYRO_DIV;
                head = (head + 1) % GES_WINDOW;
                if (head == 0) full = true;

                /* Latest tilt (0 = flat) — gates the double-tap class. */
                s_roll_deg  = atan2f(d.accel_y,
                    sqrtf(d.accel_x * d.accel_x + d.accel_z * d.accel_z)) *
                    (180.0f / 3.14159265f);
                s_pitch_deg = atan2f(d.accel_x,
                    sqrtf(d.accel_y * d.accel_y + d.accel_z * d.accel_z)) *
                    (180.0f / 3.14159265f);
            }
        }

        bool can_fire = fused_can_fire();

        /* Load the model on demand (keeps PSRAM free when inactive). */
        if (can_fire && !s_model) {
            dl::Model *m = new (std::nothrow) dl::Model(
                (const char *)gesture_espdl,
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
            ESP_LOGI(TAG, "model unloaded (fused gesture inactive)");
        }

        if (can_fire && s_model && full) {
            infer++;
            if (infer >= GES_INFER_EVERY) {
                infer = 0;
                int64_t now_ms = esp_timer_get_time() / 1000;

                /* Physical gates decide WHICH class this window could be. */
                bool wrist_like = window_is_wrist_like(ring, head, GES_WINDOW);
                bool tap_like   = window_is_tap_like(ring, head, GES_WINDOW) &&
                                  fabsf(s_roll_deg) <= GES_TILT_MAX_DEG &&
                                  fabsf(s_pitch_deg) <= GES_TILT_MAX_DEG;

                if (wrist_like || tap_like) {
                    dl::TensorBase *input  = s_model->get_input();
                    dl::TensorBase *output = s_model->get_output();
                    int8_t *in_data        = input->get_element_ptr<int8_t>();
                    float   in_inv_scale   = DL_RESCALE(input->exponent);
                    float   out_scale      = DL_SCALE(output->exponent);

                    /* Fill the int8 input with the 75 most recent samples. */
                    int idx = head;
                    for (int i = 0; i < GES_WINDOW; i++) {
                        in_data[i * 6 + 0] = dl::quantize<int8_t>(ring[idx][0], in_inv_scale);
                        in_data[i * 6 + 1] = dl::quantize<int8_t>(ring[idx][1], in_inv_scale);
                        in_data[i * 6 + 2] = dl::quantize<int8_t>(ring[idx][2], in_inv_scale);
                        in_data[i * 6 + 3] = dl::quantize<int8_t>(ring[idx][3], in_inv_scale);
                        in_data[i * 6 + 4] = dl::quantize<int8_t>(ring[idx][4], in_inv_scale);
                        in_data[i * 6 + 5] = dl::quantize<int8_t>(ring[idx][5], in_inv_scale);
                        idx = (idx + 1) % GES_WINDOW;
                    }

                    s_model->run(input, dl::RUNTIME_MODE_AUTO);

                    int8_t *oq = output->get_element_ptr<int8_t>();
                    float p_idle = dl::dequantize<int8_t, float>(oq[GES_CLASS_IDLE],  out_scale);
                    float p_wrist= dl::dequantize<int8_t, float>(oq[GES_CLASS_WRIST], out_scale);
                    float p_tap  = dl::dequantize<int8_t, float>(oq[GES_CLASS_TAP],   out_scale);
                    (void)p_idle;   /* gates already resolve the idle state */

                    float wrist_conf = (float)g_config.wrist_flick_confidence_pct / 100.0f;
                    if (wrist_conf < 0.2f)  wrist_conf = 0.2f;
                    if (wrist_conf > 0.99f) wrist_conf = 0.99f;

                    /* ── Decide ONE of the three states ────────────── */
                    bool tap   = tap_like   && p_tap   >= GES_TAP_CONFIDENCE && p_tap   >= p_wrist;
                    bool wrist = wrist_like && p_wrist >= wrist_conf         && p_wrist >= p_tap;

                    if (tap) {
                        if (now_ms >= tap_cooldown_until) {
                            if (++tap_consec >= GES_CONSECUTIVE) {
                                ESP_LOGI(TAG, ">>> DOUBLE-TAP (%.3f) — click <<<", p_tap);
                                tap_cooldown_until = now_ms + GES_TAP_COOLDOWN_MS;
                                tap_consec = 0;
                                app_events_request_vibe_click();
                                air_pointer_click();
                            }
                        }
                    } else {
                        tap_consec = 0;
                    }

                    if (wrist) {
                        if (now_ms >= wrist_cooldown_until) {
                            if (++wrist_consec >= GES_CONSECUTIVE) {
                                ESP_LOGI(TAG, ">>> WRIST FLICK (%.3f) — back <<<", p_wrist);
                                wrist_cooldown_until = now_ms + GES_WRIST_COOLDOWN_MS;
                                wrist_consec = 0;
                                app_events_request_vibe_click();
                                g_pending_reset = true;   /* same back path as GPIO0 */
                            }
                        }
                    } else {
                        wrist_consec = 0;
                    }
                } else {
                    /* Neither physical gate hit -> idle (skip the CNN). */
                    wrist_consec = 0;
                    tap_consec = 0;
                }
            }
        } else {
            wrist_consec = 0;
            tap_consec = 0;
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
#endif /* GESTURE_FUSION_MODEL_PRESENT */
