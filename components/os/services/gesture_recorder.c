/**
 * @file gesture_recorder.c
 * @brief IMU gesture data recorder (see gesture_recorder.h).
 *
 * Implementation notes:
 *  - The recording task samples hw_imu_read() at 50 Hz using
 *    vTaskDelayUntil, exactly matching the training data rate.
 *  - Raw samples are stored in a growable PSRAM buffer (doubling
 *    allocation), protected by a mutex so the UI can read counts / save.
 *  - FLICK mode: "Mark" copies the last 1.5 s (75 samples) of the raw
 *    buffer into a segment buffer.  Saving writes a CSV made ONLY of those
 *    marked windows (one flick per 75-row block) -> flick.csv.
 *  - NOTFLICK mode: saving writes the whole raw capture -> notflick.csv.
 */

#include "gesture_recorder.h"

#include "hw_imu.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "GEST_REC";

#define GR_SAMPLE_RATE_HZ   50
#define GR_INIT_CAP         2048      /* samples (~41 s) */
#define GR_MAX_CAP          60000     /* samples (~20 min) */
#define GR_FLICK_WINDOW     75        /* 1.5 s @ 50 Hz — matches training */
#define GR_MAX_FLICKS       500       /* max marked flicks per session */
#define GR_TASK_STACK       4096
#define GR_TASK_PRIO        5

typedef struct {
    float v[6];   /* ax ay az gx gy gz */
} gr_sample_t;

/* ── Module state ─────────────────────────────────────────────────── */
static TaskHandle_t      s_task      = NULL;
static SemaphoreHandle_t s_mutex     = NULL;
static volatile bool     s_recording = false;

static gesture_rec_mode_t s_mode      = GESTURE_REC_MODE_FLICK;

static gr_sample_t      *s_buf       = NULL;   /* raw continuous capture */
static uint32_t          s_cap       = 0;
static uint32_t          s_count     = 0;

static gr_sample_t      *s_segs      = NULL;   /* marked flick windows */
static uint32_t          s_seg_cap   = 0;
static uint32_t          s_seg_count = 0;

static float             s_peak_gyro = 0.0f;

static void recorder_task(void *arg);

/* ── Buffer management (call with s_mutex held) ───────────────────── */

static bool buf_ensure(uint32_t needed)
{
    if (needed <= s_cap) return true;
    if (s_cap >= GR_MAX_CAP) return false;

    uint32_t new_cap = s_cap ? s_cap : GR_INIT_CAP;
    while (new_cap < needed && new_cap < GR_MAX_CAP) new_cap *= 2;
    if (new_cap > GR_MAX_CAP) new_cap = GR_MAX_CAP;

    gr_sample_t *nb = heap_caps_realloc(
        s_buf, new_cap * sizeof(gr_sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nb) {
        ESP_LOGE(TAG, "PSRAM buffer grow to %lu samples failed", (unsigned long)new_cap);
        return false;
    }
    s_buf = nb;
    s_cap = new_cap;
    ESP_LOGI(TAG, "buffer grew to %lu samples", (unsigned long)s_cap);
    return true;
}

static bool seg_ensure(void)
{
    if (s_seg_count < s_seg_cap) return true;
    if (s_seg_cap >= GR_MAX_FLICKS) return false;

    uint32_t new_cap = s_seg_cap ? s_seg_cap * 2 : 64;
    if (new_cap > GR_MAX_FLICKS) new_cap = GR_MAX_FLICKS;

    gr_sample_t *ns = heap_caps_realloc(
        s_segs, new_cap * GR_FLICK_WINDOW * sizeof(gr_sample_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ns) {
        ESP_LOGE(TAG, "PSRAM segment grow to %lu flicks failed", (unsigned long)new_cap);
        return false;
    }
    s_segs = ns;
    s_seg_cap = new_cap;
    return true;
}

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t gesture_recorder_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (!s_task) {
        BaseType_t ok = xTaskCreate(recorder_task, "gest_rec", GR_TASK_STACK,
                                    NULL, GR_TASK_PRIO, &s_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create recorder task");
            return ESP_ERR_NO_MEM;
        }
    }
    return (s_mutex && s_task) ? ESP_OK : ESP_ERR_NO_MEM;
}

void gesture_recorder_set_mode(gesture_rec_mode_t mode)
{
    s_mode = (mode == GESTURE_REC_MODE_NOTFLICK) ? GESTURE_REC_MODE_NOTFLICK
                                                 : GESTURE_REC_MODE_FLICK;
}

gesture_rec_mode_t gesture_recorder_get_mode(void)
{
    return s_mode;
}

bool gesture_recorder_start(void)
{
    if (s_recording) return true;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    s_count     = 0;
    s_seg_count = 0;
    s_peak_gyro = 0.0f;
    buf_ensure(GR_INIT_CAP);
    seg_ensure();
    xSemaphoreGive(s_mutex);

    s_recording = true;
    ESP_LOGI(TAG, "Recording started (mode=%s)",
             s_mode == GESTURE_REC_MODE_FLICK ? "flick" : "notflick");
    return true;
}

void gesture_recorder_stop(void)
{
    s_recording = false;
    ESP_LOGI(TAG, "Recording stopped — %lu samples, %lu flicks",
             (unsigned long)s_count, (unsigned long)s_seg_count);
}

bool gesture_recorder_is_recording(void)
{
    return s_recording;
}

bool gesture_recorder_mark_flick(void)
{
    bool ok = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    if (s_recording && s_mode == GESTURE_REC_MODE_FLICK &&
        s_count >= GR_FLICK_WINDOW && seg_ensure()) {
        gr_sample_t *dst = &s_segs[(size_t)s_seg_count * GR_FLICK_WINDOW];
        memcpy(dst, &s_buf[s_count - GR_FLICK_WINDOW],
               GR_FLICK_WINDOW * sizeof(gr_sample_t));
        s_seg_count++;
        ok = true;
    }
    xSemaphoreGive(s_mutex);
    if (ok) ESP_LOGI(TAG, "flick #%lu marked", (unsigned long)s_seg_count);
    return ok;
}

uint32_t gesture_recorder_flick_count(void)
{
    uint32_t n = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        n = s_seg_count;
        xSemaphoreGive(s_mutex);
    }
    return n;
}

uint32_t gesture_recorder_sample_count(void)
{
    uint32_t n = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        n = s_count;
        xSemaphoreGive(s_mutex);
    }
    return n;
}

float gesture_recorder_peak_gyro(void)
{
    float p = 0.0f;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        p = s_peak_gyro;
        xSemaphoreGive(s_mutex);
    }
    return p;
}

void gesture_recorder_clear(void)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    s_count     = 0;
    s_seg_count = 0;
    s_peak_gyro = 0.0f;
    xSemaphoreGive(s_mutex);
}

bool gesture_recorder_sd_ready(void)
{
    struct stat st;
    return (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode));
}

esp_err_t gesture_recorder_save(const char *filename)
{
    if (!filename || !*filename) return ESP_ERR_INVALID_ARG;
    if (!gesture_recorder_sd_ready()) {
        ESP_LOGW(TAG, "SD card not mounted");
        return ESP_ERR_NOT_FOUND;
    }

    /* Make sure the target directory exists. */
    mkdir(GESTURE_REC_DIR, 0777);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t rows;
    if (s_mode == GESTURE_REC_MODE_FLICK) {
        rows = s_seg_count * GR_FLICK_WINDOW;
        if (s_seg_count == 0) {
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "no flicks marked — tap Mark after each flick");
            return ESP_ERR_NO_MEM;
        }
    } else {
        rows = s_count;
        if (s_count == 0) {
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "nothing recorded yet");
            return ESP_ERR_NO_MEM;
        }
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", GESTURE_REC_DIR, filename);

    FILE *f = fopen(path, "w");
    if (!f) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "cannot open %s for writing", path);
        return ESP_FAIL;
    }

    fprintf(f, "ax,ay,az,gx,gy,gz\n");
    if (s_mode == GESTURE_REC_MODE_FLICK) {
        /* FLICK: each marked 1.5 s window, one after another. */
        for (uint32_t i = 0; i < s_seg_count; i++) {
            const gr_sample_t *seg = &s_segs[(size_t)i * GR_FLICK_WINDOW];
            for (uint32_t j = 0; j < GR_FLICK_WINDOW; j++) {
                const gr_sample_t *s = &seg[j];
                fprintf(f, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                        (double)s->v[0], (double)s->v[1], (double)s->v[2],
                        (double)s->v[3], (double)s->v[4], (double)s->v[5]);
            }
        }
    } else {
        /* NOTFLICK: the whole continuous capture. */
        for (uint32_t i = 0; i < s_count; i++) {
            const gr_sample_t *s = &s_buf[i];
            fprintf(f, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                    (double)s->v[0], (double)s->v[1], (double)s->v[2],
                    (double)s->v[3], (double)s->v[4], (double)s->v[5]);
        }
    }
    int r = fclose(f);

    xSemaphoreGive(s_mutex);

    if (r != 0) {
        ESP_LOGE(TAG, "error finalising %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved %lu rows (%lu flicks) -> %s",
             (unsigned long)rows,
             s_mode == GESTURE_REC_MODE_FLICK ? (unsigned long)s_seg_count : 0UL,
             path);
    return ESP_OK;
}

/* ── Recording task ───────────────────────────────────────────────── */

static void recorder_task(void *arg)
{
    (void)arg;
    hw_imu_data_t d;

    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / GR_SAMPLE_RATE_HZ);

    for (;;) {
        if (s_recording && hw_imu_read(&d) == ESP_OK) {
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (buf_ensure(s_count + 1)) {
                    gr_sample_t *s = &s_buf[s_count];
                    s->v[0] = d.accel_x; s->v[1] = d.accel_y; s->v[2] = d.accel_z;
                    s->v[3] = d.gyro_x;  s->v[4] = d.gyro_y;  s->v[5] = d.gyro_z;
                    float mag = sqrtf(d.gyro_x * d.gyro_x +
                                      d.gyro_y * d.gyro_y +
                                      d.gyro_z * d.gyro_z);
                    if (mag > s_peak_gyro) s_peak_gyro = mag;
                    s_count++;
                } else {
                    /* Buffer full — stop recording. */
                    s_recording = false;
                    ESP_LOGW(TAG, "buffer full, recording auto-stopped");
                }
                xSemaphoreGive(s_mutex);
            }
            vTaskDelayUntil(&last, period);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
