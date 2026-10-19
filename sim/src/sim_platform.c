/**
 * @file sim_platform.c
 * @brief Host implementations of the ESP-IDF / FreeRTOS APIs the UI uses,
 *        plus synthetic sensor values.
 *
 * Everything here is a *simulation*: battery percentage, charger state,
 * temperatures and IMU readings are made up so the UI has plausible data.
 * Nothing is read from real hardware.
 */
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "sim_platform.h"

/* ── Logging ──────────────────────────────────────────────────────── */
static int s_log_level = -1;

int sim_log_enabled(esp_log_level_t level)
{
    if (s_log_level < 0) {
        const char *env = getenv("SIM_LOG");
        s_log_level = env ? atoi(env) : ESP_LOG_INFO;
    }
    return (int)level <= s_log_level;
}

void sim_log_level_set_all(esp_log_level_t level)
{
    s_log_level = level;
}

void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    (void)tag;
    s_log_level = level;
}

void sim_log_write(esp_log_level_t level, const char *tag,
                   const char *file, int line, const char *fmt, ...)
{
    if (!sim_log_enabled(level)) return;

    static const char *names[] = { "NONE", "E", "W", "I", "D", "V" };
    static const char *colors[] = { "", "\033[31m", "\033[33m",
                                    "\033[32m", "\033[36m", "\033[90m" };
    (void)file; (void)line;

    fprintf(stderr, "%s%s (%s)\033[0m ", colors[level], names[level],
            tag ? tag : "-");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ── esp_err ──────────────────────────────────────────────────────── */
const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:   return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    default:       return "ESP_ERR";
    }
}

const char *esp_err_to_name_r(esp_err_t code, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%s", esp_err_to_name(code));
    return buf;
}

/* ── Timing ───────────────────────────────────────────────────────── */
int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

struct esp_timer {
    esp_timer_cb_t cb;
    void *arg;
    uint64_t period_us;
    uint64_t next_us;
    bool active;
    bool periodic;
    struct esp_timer *next;   /**< global list for the main-loop pump */
};

static struct esp_timer *s_timer_head;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *out)
{
    if (!args || !out) return ESP_ERR_INVALID_ARG;
    struct esp_timer *t = (struct esp_timer *)calloc(1, sizeof(*t));
    if (!t) return ESP_ERR_NO_MEM;
    t->cb = args->callback;
    t->arg = args->arg;
    t->next = s_timer_head;
    s_timer_head = t;
    *out = t;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    t->period_us = timeout_us;
    t->next_us   = (uint64_t)esp_timer_get_time() + timeout_us;
    t->active    = true;
    t->periodic  = false;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period_us)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    t->period_us = period_us;
    t->next_us   = (uint64_t)esp_timer_get_time() + period_us;
    t->active    = true;
    t->periodic  = true;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    t->active = false;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    struct esp_timer **pp = &s_timer_head;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp) *pp = t->next;
    free(t);
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t t)
{
    return t && t->active;
}

/** Run any expired esp_timers. Called from the main loop. */
void sim_platform_run_timers(void);

/* ── System ───────────────────────────────────────────────────────── */
void esp_restart(void)
{
    fprintf(stderr, "[sim] esp_restart() — exiting\n");
    exit(0);
}

uint32_t esp_get_free_heap_size(void)          { return 8u * 1024u * 1024u; }
uint32_t esp_get_minimum_free_heap_size(void)  { return 6u * 1024u * 1024u; }

static uint32_t s_rand_state = 0x12345678u;
uint32_t esp_random(void)
{
    s_rand_state = s_rand_state * 1664525u + 1013904223u;
    return s_rand_state;
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) p[i] = (uint8_t)esp_random();
}

const char *esp_get_idf_version(void) { return "sim"; }

/* ── FreeRTOS ─────────────────────────────────────────────────────── */
void vTaskDelay(TickType_t ticks)
{
    struct timespec ts = { 0, (long)(ticks % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void vTaskDelayUntil(TickType_t *prev, TickType_t increment)
{
    vTaskDelay(increment);
    *prev += increment;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(esp_timer_get_time() / 1000);
}

typedef struct {
    TaskFunction_t fn;
    void         *arg;
} sim_task_t;

static void *sim_task_trampoline(void *p)
{
    sim_task_t t = *(sim_task_t *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t prio, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)prio;

    /* Real detached pthread: some firmware tasks (e.g. the Activity IMU
     * sampler) loop until their run flag is cleared, so running the body
     * inline would never return from xTaskCreate(). */
    if (!fn) return 0;

    sim_task_t *t = (sim_task_t *)malloc(sizeof(*t));
    if (!t) return 0;
    t->fn  = fn;
    t->arg = arg;

    pthread_t th;
    if (pthread_create(&th, NULL, sim_task_trampoline, t) != 0) {
        free(t);
        return 0;
    }
    pthread_detach(th);

    if (out) *out = (TaskHandle_t)1;
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core)
{
    (void)core;
    return xTaskCreate(fn, name, stack, arg, prio, out);
}

void vTaskDelete(TaskHandle_t t) { (void)t; }
void vTaskSuspend(TaskHandle_t t) { (void)t; }
void vTaskResume(TaskHandle_t t) { (void)t; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }
const char *pcTaskGetName(TaskHandle_t t) { (void)t; return "sim"; }
uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 8192; }
void taskYIELD(void) { }

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    if (!m) return NULL;
    pthread_mutex_init(m, NULL);
    return (SemaphoreHandle_t)m;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    if (!m) return NULL;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return (SemaphoreHandle_t)m;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    if (!m) return NULL;
    pthread_mutex_init(m, NULL);
    pthread_mutex_lock(m);
    return (SemaphoreHandle_t)m;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    if (!s) return;
    pthread_mutex_destroy((pthread_mutex_t *)s);
    free(s);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    (void)ticks;
    if (!s) return pdFALSE;
    return pthread_mutex_lock((pthread_mutex_t *)s) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (!s) return pdFALSE;
    return pthread_mutex_unlock((pthread_mutex_t *)s) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks)
{
    return xSemaphoreTake(s, ticks);
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s)
{
    return xSemaphoreGive(s);
}

BaseType_t xSemaphoreTakeFromISR(SemaphoreHandle_t s, BaseType_t *hp)
{
    if (hp) *hp = pdFALSE;
    return xSemaphoreTake(s, 0);
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *hp)
{
    if (hp) *hp = pdFALSE;
    return xSemaphoreGive(s);
}

/* Queues — a simple fixed-capacity ring per queue. */
struct sim_queue {
    size_t item_size;
    size_t capacity;
    size_t head, tail, count;
    uint8_t *data;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct sim_queue *q = (struct sim_queue *)calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->item_size = item_size;
    q->capacity  = length;
    q->data      = (uint8_t *)calloc(length, item_size);
    if (!q->data) { free(q); return NULL; }
    return (QueueHandle_t)q;
}

void vQueueDelete(QueueHandle_t h)
{
    struct sim_queue *q = (struct sim_queue *)h;
    if (!q) return;
    free(q->data);
    free(q);
}

BaseType_t xQueueSendToBack(QueueHandle_t h, const void *item, TickType_t ticks)
{
    (void)ticks;
    struct sim_queue *q = (struct sim_queue *)h;
    if (!q || q->count >= q->capacity) return pdFALSE;
    memcpy(q->data + q->tail * q->item_size, item, q->item_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    return xQueueSendToBack(q, item, ticks);
}

BaseType_t xQueueSendToFront(QueueHandle_t h, const void *item, TickType_t ticks)
{
    (void)ticks;
    struct sim_queue *q = (struct sim_queue *)h;
    if (!q || q->count >= q->capacity) return pdFALSE;
    q->head = (q->head + q->capacity - 1) % q->capacity;
    memcpy(q->data + q->head * q->item_size, item, q->item_size);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t h, void *item, TickType_t ticks)
{
    (void)ticks;
    struct sim_queue *q = (struct sim_queue *)h;
    if (!q || q->count == 0) return pdFALSE;
    memcpy(item, q->data + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return pdTRUE;
}

BaseType_t xQueuePeek(QueueHandle_t h, void *item, TickType_t ticks)
{
    (void)ticks;
    struct sim_queue *q = (struct sim_queue *)h;
    if (!q || q->count == 0) return pdFALSE;
    memcpy(item, q->data + q->head * q->item_size, q->item_size);
    return pdTRUE;
}

BaseType_t xQueueReset(QueueHandle_t h)
{
    struct sim_queue *q = (struct sim_queue *)h;
    if (!q) return pdFALSE;
    q->head = q->tail = q->count = 0;
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t h)
{
    struct sim_queue *q = (struct sim_queue *)h;
    return q ? (UBaseType_t)q->count : 0;
}

/* Event groups */
struct sim_eg { EventBits_t bits; };

EventGroupHandle_t xEventGroupCreate(void)
{
    return (EventGroupHandle_t)calloc(1, sizeof(struct sim_eg));
}

void vEventGroupDelete(EventGroupHandle_t g) { free(g); }

EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits)
{
    struct sim_eg *e = (struct sim_eg *)g;
    if (!e) return 0;
    e->bits |= bits;
    return e->bits;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits)
{
    struct sim_eg *e = (struct sim_eg *)g;
    if (!e) return 0;
    e->bits &= ~bits;
    return e->bits;
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t g)
{
    struct sim_eg *e = (struct sim_eg *)g;
    return e ? e->bits : 0;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits,
                                BaseType_t clear_on_exit, BaseType_t wait_all,
                                TickType_t ticks)
{
    (void)ticks;
    struct sim_eg *e = (struct sim_eg *)g;
    if (!e) return 0;
    EventBits_t got = wait_all ? (e->bits & bits) : (e->bits & bits);
    if (clear_on_exit) e->bits &= ~bits;
    return got;
}

/* Timers — one-shot/periodic xTimer are pumped by sim_platform_run_timers. */
struct sim_xtimer {
    TimerCallbackFunction_t cb;
    TickType_t period;
    TickType_t next;
    bool active;
    bool reload;
};

TimerHandle_t xTimerCreate(const char *name, TickType_t period,
                           UBaseType_t reload, void *id,
                           TimerCallbackFunction_t cb)
{
    (void)name; (void)id;
    struct sim_xtimer *t = (struct sim_xtimer *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cb = cb;
    t->period = period;
    t->reload = reload != 0;
    return (TimerHandle_t)t;
}

BaseType_t xTimerStart(TimerHandle_t h, TickType_t wait)
{
    (void)wait;
    struct sim_xtimer *t = (struct sim_xtimer *)h;
    if (!t) return pdFALSE;
    t->active = true;
    t->next = xTaskGetTickCount() + t->period;
    return pdTRUE;
}

BaseType_t xTimerStop(TimerHandle_t h, TickType_t wait)
{
    (void)wait;
    struct sim_xtimer *t = (struct sim_xtimer *)h;
    if (!t) return pdFALSE;
    t->active = false;
    return pdTRUE;
}

BaseType_t xTimerChangePeriod(TimerHandle_t h, TickType_t period, TickType_t wait)
{
    (void)wait;
    struct sim_xtimer *t = (struct sim_xtimer *)h;
    if (!t) return pdFALSE;
    t->period = period;
    t->next = xTaskGetTickCount() + period;
    return pdTRUE;
}

BaseType_t xTimerReset(TimerHandle_t h, TickType_t wait)
{
    return xTimerStart(h, wait);
}

BaseType_t xTimerDelete(TimerHandle_t h, TickType_t wait)
{
    (void)wait;
    free(h);
    return pdTRUE;
}

/* ── Synthetic sensors ────────────────────────────────────────────── */
#define SIM_START_BATTERY 87

static int   s_battery_pct      = SIM_START_BATTERY;
static bool  s_charging         = false;
static float s_chip_temp        = 31.5f;
static float s_batt_temp        = 29.0f;
static float s_tilt_x           = 0.0f;
static float s_tilt_y           = 0.0f;
static float s_noise_phase      = 0.0f;
static bool  s_use_real_time    = true;
static uint32_t s_last_tick_ms  = 0;
static bool  s_raise_pulse      = false;
static uint32_t s_raise_until   = 0;

int sim_battery_pct(void)      { return s_battery_pct; }
bool sim_battery_charging(void) { return s_charging; }
float sim_battery_voltage(void)
{
    /* 3.3 V empty .. 4.2 V full, with a small load sag. */
    return 3.3f + (s_battery_pct / 100.0f) * 0.9f - (s_charging ? 0.0f : 0.03f);
}
float sim_chip_temp_c(void)     { return s_chip_temp; }
float sim_battery_temp_c(void)  { return s_batt_temp; }

void sim_set_tilt(float x_deg, float y_deg)
{
    s_tilt_x = x_deg;
    s_tilt_y = y_deg;
}

void sim_get_tilt(float *x_deg, float *y_deg)
{
    if (x_deg) *x_deg = s_tilt_x;
    if (y_deg) *y_deg = s_tilt_y;
}

void sim_get_accel(float *x, float *y, float *z)
{
    /* Gravity projected by the tilt, plus a little noise. */
    const float g = 1.0f;
    const float rad_x = s_tilt_x * 0.0174533f;
    const float rad_y = s_tilt_y * 0.0174533f;
    const float noise = 0.01f * (float)((s_noise_phase > 1.0f) ? 1.0 : -1.0);

    if (x) *x = g * sinf(rad_x) + noise;
    if (y) *y = g * sinf(rad_y) - noise;
    if (z) *z = g * cosf(rad_x) * cosf(rad_y);
}

void sim_raise_wrist(void)
{
    s_raise_pulse = true;
    s_raise_until = xTaskGetTickCount() + 400;
}

void sim_set_use_real_time(bool real)
{
    s_use_real_time = real;
}

bool sim_use_real_time(void) { return s_use_real_time; }

/* Drain any expired esp_timers; called once per main-loop iteration. */
void sim_platform_run_timers(void)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    for (struct esp_timer *t = s_timer_head; t; t = t->next) {
        if (!t->active || now < t->next_us) continue;
        if (t->periodic) {
            t->next_us = now + t->period_us;
        } else {
            t->active = false;
        }
        if (t->cb) t->cb(t->arg);
    }
}

/* Advance the simulated world. */
void sim_platform_tick(uint32_t dt_ms)
{
    const uint32_t prev = s_last_tick_ms;
    s_last_tick_ms += dt_ms;
    (void)prev;

    /* Battery: discharge ~1 % per 3 minutes, charge faster. */
    static uint32_t s_batt_accum_ms = 0;
    s_batt_accum_ms += dt_ms;
    if (s_batt_accum_ms >= 180000u) {
        s_batt_accum_ms = 0;
        if (s_charging) {
            if (s_battery_pct < 100) s_battery_pct++;
        } else if (s_battery_pct > 0) {
            s_battery_pct--;
        }
    }

    /* Temperature drifts slowly around a comfortable value. */
    s_chip_temp += ((31.5f - s_chip_temp) * 0.0005f);
    s_batt_temp += ((29.0f - s_batt_temp) * 0.0005f);

    s_noise_phase += 0.01f;
    if (s_noise_phase > 2.0f) s_noise_phase = 0.0f;
}

bool sim_raise_wrist_active(void)
{
    return s_raise_pulse && xTaskGetTickCount() < s_raise_until;
}
