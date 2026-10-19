/**
 * @file wake_log.h
 * @brief Persistent wake-event logger for light-sleep / deep-sleep diagnosis.
 *
 * Records EVERY light-sleep wake (source, sleep duration, decision and CNN
 * result) into a ring in RTC fast memory so the history survives light sleep
 * AND warm resets.  On the next boot (or whenever the serial monitor is able
 * to reconnect) the whole history is dumped to the log — so you can see how
 * often / why the watch woke even when the USB-Serial/JTAG console dropped
 * during light sleep and would otherwise need a replug to show anything.
 *
 * The state is additionally persisted to SPIFFS (/spiffs/var/lib/wake_log.bin) on
 * boot and on each display-wake so the counters survive a full power cycle
 * too (e.g. if the battery dies while the watch is "asleep").
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Cause codes (mirror esp_sleep_wakeup_cause_t, kept small) ─────── */
#define WAKE_LOG_CAUSE_TIMER   4   /* ESP_SLEEP_WAKEUP_TIMER (housekeeping) */
#define WAKE_LOG_CAUSE_ULP     6   /* ESP_SLEEP_WAKEUP_ULP   (gate fired)   */
#define WAKE_LOG_CAUSE_GPIO    7   /* ESP_SLEEP_WAKEUP_GPIO  (button/INT)   */

/* ── Decision codes ────────────────────────────────────────────────── */
#define WAKE_LOG_DEC_RE_SLEEP  0   /* went straight back to light sleep */
#define WAKE_LOG_DEC_WAKE      1   /* display woken                     */

#define WAKE_LOG_MAX_RECORDS   24

typedef struct {
    uint32_t seq;           /* wake sequence number (monotonic)       */
    uint32_t uptime_ms;     /* esp_timer uptime at this wake (ms)     */
    uint32_t slept_ms;      /* time since the previous wake (~sleep)  */
    uint8_t  cause;         /* WAKE_LOG_CAUSE_*                       */
    uint8_t  decision;      /* WAKE_LOG_DEC_*                         */
    uint8_t  cnn_ran;       /* 1 = CNN inference ran on this wake     */
    uint8_t  cnn_raise;     /* 1 = CNN confirmed a raise              */
    uint16_t cnn_prob_q8;   /* CNN probability * 256                  */
    uint16_t ulp_samples;   /* low 16 bits of ULP IMU sample count    */
} wake_log_record_t;

/**
 * @brief Initialise the wake log (call after config/SPIFFS init).
 *
 * If the RTC fast-memory copy is intact it is kept (authoritative).  If not
 * (fresh boot / power cycle) it is restored from SPIFFS when available.
 */
void wake_log_init(void);

/**
 * @brief Record one wake event (cheap; call once per light-sleep wake).
 *
 * @return This wake's sequence number (for logging), or 0 on failure.
 */
uint32_t wake_log_add(uint8_t cause, uint8_t decision, uint8_t cnn_ran,
                      uint8_t cnn_raise, uint16_t cnn_prob_q8,
                      uint32_t ulp_samples);

/**
 * @brief Print the full wake history + summary via ESP_LOGI.
 */
void wake_log_dump(void);

/**
 * @brief Persist the current state to SPIFFS (/spiffs/var/lib/wake_log.bin).
 */
void wake_log_persist(void);

/**
 * @brief Wipe the log (counters + ring + SPIFFS copy).
 */
void wake_log_reset(void);

/**
 * @brief Total number of wake events recorded since the log was started.
 */
uint32_t wake_log_total_wakes(void);

/** @brief Total wake events per cause (TIMER / ULP-gate / GPIO). */
uint32_t wake_log_timer_wakes(void);
uint32_t wake_log_ulp_wakes(void);
uint32_t wake_log_gpio_wakes(void);

/** @brief How many wakes ran the tinyML (ESP-DL CNN). */
uint32_t wake_log_cnn_runs(void);

/** @brief How many times tinyML confirmed a raise → display woken. */
uint32_t wake_log_raises(void);

/** @brief How many times tinyML REJECTED a wake (woke up, then went
 *         straight back to sleep).  This is the "woke but didn't pass
 *         tinyML" number. */
uint32_t wake_log_rejects(void);

#ifdef __cplusplus
}
#endif
