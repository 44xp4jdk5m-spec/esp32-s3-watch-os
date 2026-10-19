/**
 * @file wake_log.c
 * @brief Persistent wake-event logger — see wake_log.h.
 *
 * State lives in RTC fast memory (`.rtc.force_fast`) so it survives light
 * sleep AND warm resets, and is mirrored to SPIFFS on boot / display-wake so
 * it survives a full power cycle.  All access is serialised with a mutex
 * (wake events are recorded from the main loop; the SPIFFS copy is written
 * from the main loop too, so no cross-task SPIFFS races).
 */

#include "wake_log.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define WAKE_LOG_TAG        "WAKE_LOG"
#define WAKE_LOG_MAGIC      0x57414B45u            /* "WAKE" */
#define WAKE_LOG_FILE       "/spiffs/var/lib/wake_log.bin"

typedef struct {
    uint32_t magic;             /* WAKE_LOG_MAGIC when valid      */
    uint32_t total_wakes;       /* cumulative wake count          */
    uint32_t timer_wakes;       /* by cause                       */
    uint32_t ulp_wakes;
    uint32_t gpio_wakes;
    uint32_t cnn_runs;          /* CNN inferences executed        */
    uint32_t raises_confirmed;  /* CNN-confirmed raises           */
    uint32_t cnn_rejects;       /* CNN ran but rejected → re-sleep */
    uint32_t last_wake_ms;      /* uptime of the previous wake    */
    uint32_t head;              /* next ring slot                 */
    uint32_t filled;            /* ring entries written           */
    wake_log_record_t ring[WAKE_LOG_MAX_RECORDS];
} wake_log_state_t;

/* In RTC fast memory: survives light sleep AND warm resets. */
static RTC_FAST_ATTR wake_log_state_t s_state;

static SemaphoreHandle_t s_lock = NULL;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void wake_log_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void wake_log_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void wake_log_init(void)
{
    wake_log_lock();

    if (s_state.magic == WAKE_LOG_MAGIC) {
        /* RTC copy intact — it is the most recent, keep it. */
        ESP_LOGI(WAKE_LOG_TAG, "wake log: %" PRIu32 " wakes on record (RTC)",
                 s_state.total_wakes);
        wake_log_unlock();
        return;
    }

    /* Fresh boot / power cycle: try to restore from SPIFFS. */
    memset(&s_state, 0, sizeof(s_state));
    FILE *f = fopen(WAKE_LOG_FILE, "rb");
    if (f) {
        size_t rd = fread(&s_state, 1, sizeof(s_state), f);
        fclose(f);
        if (rd == sizeof(s_state) && s_state.magic == WAKE_LOG_MAGIC) {
            ESP_LOGI(WAKE_LOG_TAG,
                     "wake log restored from SPIFFS: %" PRIu32 " wakes on record",
                     s_state.total_wakes);
            wake_log_unlock();
            return;
        }
        memset(&s_state, 0, sizeof(s_state));
    }

    s_state.magic = WAKE_LOG_MAGIC;
    ESP_LOGI(WAKE_LOG_TAG, "wake log started (fresh)");
    wake_log_unlock();
}

uint32_t wake_log_add(uint8_t cause, uint8_t decision, uint8_t cnn_ran,
                      uint8_t cnn_raise, uint16_t cnn_prob_q8,
                      uint32_t ulp_samples)
{
    uint32_t seq = 0;
    wake_log_lock();

    if (s_state.magic != WAKE_LOG_MAGIC) {
        s_state.magic = WAKE_LOG_MAGIC;
    }

    wake_log_state_t *st = &s_state;
    st->total_wakes++;
    switch (cause) {
        case WAKE_LOG_CAUSE_TIMER: st->timer_wakes++; break;
        case WAKE_LOG_CAUSE_ULP:   st->ulp_wakes++;   break;
        default:                   st->gpio_wakes++;  break;
    }
    if (cnn_ran) {
        st->cnn_runs++;
        if (cnn_raise) {
            st->raises_confirmed++;
        } else {
            st->cnn_rejects++;
        }
    }

    uint32_t now = now_ms();
    wake_log_record_t *r = &st->ring[st->head];
    r->seq          = st->total_wakes;
    r->uptime_ms    = now;
    r->slept_ms     = (st->last_wake_ms) ? (now - st->last_wake_ms) : 0;
    r->cause        = cause;
    r->decision     = decision;
    r->cnn_ran      = cnn_ran;
    r->cnn_raise    = cnn_raise;
    r->cnn_prob_q8  = cnn_prob_q8;
    r->ulp_samples  = (uint16_t)(ulp_samples & 0xFFFFu);
    st->last_wake_ms = now;

    st->head = (st->head + 1) % WAKE_LOG_MAX_RECORDS;
    if (st->filled < WAKE_LOG_MAX_RECORDS) {
        st->filled++;
    }
    seq = st->total_wakes;

    wake_log_unlock();
    return seq;
}

void wake_log_dump(void)
{
    wake_log_lock();
    wake_log_state_t *st = &s_state;

    ESP_LOGI(WAKE_LOG_TAG, "═══════════════════ WAKE LOG ═══════════════════");
    ESP_LOGI(WAKE_LOG_TAG,
             "Total wakes: %" PRIu32 "   (TIMER %" PRIu32 " / ULP-gate %" PRIu32
             " / GPIO %" PRIu32 ")",
             st->total_wakes, st->timer_wakes, st->ulp_wakes, st->gpio_wakes);
    ESP_LOGI(WAKE_LOG_TAG, "CNN runs: %" PRIu32 "   raises: %" PRIu32
                           "   rejected: %" PRIu32,
             st->cnn_runs, st->raises_confirmed, st->cnn_rejects);

    if (st->filled == 0) {
        ESP_LOGI(WAKE_LOG_TAG, "No wakes recorded yet.");
    } else {
        ESP_LOGI(WAKE_LOG_TAG, "Last %" PRIu32 " wakes (newest first):",
                 st->filled);
        for (uint32_t i = 0; i < st->filled; i++) {
            uint32_t idx = (st->head + WAKE_LOG_MAX_RECORDS - 1 - i)
                           % WAKE_LOG_MAX_RECORDS;
            const wake_log_record_t *r = &st->ring[idx];
            const char *c = (r->cause == WAKE_LOG_CAUSE_TIMER) ? "TIMER" :
                            (r->cause == WAKE_LOG_CAUSE_ULP)   ? "ULP-gate" :
                                                                 "GPIO";
            if (r->cnn_ran) {
                ESP_LOGI(WAKE_LOG_TAG,
                         "  #%" PRIu32 " %-8s slept %5" PRIu32 "ms imu=%5u "
                         "cnn=%.3f %s",
                         r->seq, c, r->slept_ms, (unsigned)r->ulp_samples,
                         (double)(r->cnn_prob_q8 / 256.0f),
                         r->decision ? "WAKE" : "re-sleep");
            } else {
                ESP_LOGI(WAKE_LOG_TAG,
                         "  #%" PRIu32 " %-8s slept %5" PRIu32 "ms -> %s",
                         r->seq, c, r->slept_ms,
                         r->decision ? "WAKE" : "re-sleep");
            }
        }
    }
    ESP_LOGI(WAKE_LOG_TAG, "═══════════════════════════════════════════════");
    wake_log_unlock();
}

void wake_log_persist(void)
{
    wake_log_lock();
    FILE *f = fopen(WAKE_LOG_FILE, "wb");
    if (f) {
        size_t wr = fwrite(&s_state, 1, sizeof(s_state), f);
        fclose(f);
        if (wr != sizeof(s_state)) {
            ESP_LOGW(WAKE_LOG_TAG, "persist wrote %u/%u bytes", (unsigned)wr,
                     (unsigned)sizeof(s_state));
        }
    } else {
        ESP_LOGW(WAKE_LOG_TAG, "persist failed — SPIFFS not mounted?");
    }
    wake_log_unlock();
}

void wake_log_reset(void)
{
    wake_log_lock();
    memset(&s_state, 0, sizeof(s_state));
    s_state.magic = WAKE_LOG_MAGIC;
    wake_log_unlock();
    wake_log_persist();
}

uint32_t wake_log_total_wakes(void)
{
    return s_state.total_wakes;
}

uint32_t wake_log_timer_wakes(void)
{
    return s_state.timer_wakes;
}

uint32_t wake_log_ulp_wakes(void)
{
    return s_state.ulp_wakes;
}

uint32_t wake_log_gpio_wakes(void)
{
    return s_state.gpio_wakes;
}

uint32_t wake_log_cnn_runs(void)
{
    return s_state.cnn_runs;
}

uint32_t wake_log_raises(void)
{
    return s_state.raises_confirmed;
}

uint32_t wake_log_rejects(void)
{
    return s_state.cnn_rejects;
}
