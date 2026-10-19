/**
 * @file hw_sleep.c
 * @brief Light-sleep entry/exit with GPIO + timer wake sources (ESP32-S3).
 *
 * The CPU is fully halted during light sleep (RAM retained), so power draw
 * drops to the light-sleep level (~0.5-2 mA depending on PSRAM/flash/PMIC
 * states).  The touch panel INT, both buttons and an optional housekeeping
 * timer can wake the chip; any of them resumes the main loop and normal
 * input polling takes over.
 */

#include "hw_sleep.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

static const char *TAG = "HW_SLEEP";

/* ULP-mode flags (owned by the os layer via the setters below). */
static bool s_keep_imu    = false;  /* keep QMI8658 on in standby (ULP samples it) */
static bool s_ulp_wakeup  = false;  /* add ULP coprocessor wake source */

/* Housekeeping timer wake period for the next light sleep (0 = off).  The
 * power manager sets this right before each sleep so timer wakes only fire
 * when work is due (top-of-hour battery logging) — not on a constant
 * schedule. */
static uint64_t s_housekeep_period_us = HW_SLEEP_HOUSEKEEP_PERIOD_US;

void hw_sleep_set_keep_imu(bool keep)   { s_keep_imu = keep; }
bool hw_sleep_get_keep_imu(void)        { return s_keep_imu; }
void hw_sleep_set_ulp_wakeup(bool enable) { s_ulp_wakeup = enable; }
void hw_sleep_set_housekeep_period_us(uint64_t us) { s_housekeep_period_us = us; }

/* ── Normal (non-sleep) pin config for the wake pins ─────────────────── */

static void hw_sleep_restore_pins(void)
{
    /* GPIO0 (BOOT): input + pull-up (matches hw_button.c). */
    gpio_config_t boot = {
        .pin_bit_mask  = BIT64(HW_SLEEP_WAKE_BOOT_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot);

    /* GPIO10 (PWR): floating input ONLY — PEK is handled by the AXP2101.
     * Pull-ups here would keep the PMIC awake after shutdown. */
    gpio_config_t pwr = {
        .pin_bit_mask  = BIT64(HW_SLEEP_WAKE_PWR_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr);

    /* GPIO38 (touch INT): plain input.  The touch controller drives it
     * low on touch (open-drain, external pull-up); not actively polled. */
    gpio_config_t touch = {
        .pin_bit_mask  = BIT64(HW_SLEEP_WAKE_TOUCH_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&touch);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void hw_sleep_light_exit(void)
{
    gpio_wakeup_disable(HW_SLEEP_WAKE_TOUCH_GPIO);
    gpio_wakeup_disable(HW_SLEEP_WAKE_BOOT_GPIO);
    gpio_wakeup_disable(HW_SLEEP_WAKE_PWR_GPIO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    hw_sleep_restore_pins();
}

esp_sleep_wakeup_cause_t hw_sleep_light_enter(void)
{
    esp_err_t ret;
    esp_sleep_wakeup_cause_t cause = ESP_SLEEP_WAKEUP_UNDEFINED;

    /* Re-arm the pins as inputs (a previous sleep may have left them in a
     * wake configuration; harmless to reconfigure). */
    hw_sleep_restore_pins();

    /* Touch INT (active low) — the FT3168 pulls this low while touched and
     * it stays low in its low-power monitor mode. */
    ret = gpio_wakeup_enable(HW_SLEEP_WAKE_TOUCH_GPIO, GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch wake enable failed: %s", esp_err_to_name(ret));
        return cause;   /* ESP_SLEEP_WAKEUP_UNDEFINED — sleep never entered */
    }
    /* BOOT button (active low). */
    ret = gpio_wakeup_enable(HW_SLEEP_WAKE_BOOT_GPIO, GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BOOT wake enable failed: %s", esp_err_to_name(ret));
        goto err;
    }
    /* PWR button (active high, via AXP2101 PEK). */
    ret = gpio_wakeup_enable(HW_SLEEP_WAKE_PWR_GPIO, GPIO_INTR_HIGH_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PWR wake enable failed: %s", esp_err_to_name(ret));
        goto err;
    }
    ret = esp_sleep_enable_gpio_wakeup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio wakeup enable failed: %s", esp_err_to_name(ret));
        goto err;
    }

    /* Housekeeping timer wake (top-of-hour battery logging).  The period is
     * dynamic — the power manager computes the delay until the next real
     * task before every sleep. */
    if (s_housekeep_period_us > 0) {
        ret = esp_sleep_enable_timer_wakeup(s_housekeep_period_us);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "timer wake enable failed: %s", esp_err_to_name(ret));
            /* Non-fatal — GPIO wake alone still works. */
        }
    }

    /* ULP coprocessor wake (wake-on-raise gate).  The ULP program must have
     * been loaded + started (ulp_riscv_load_binary/run) by the caller.
     * Keep the RTC_PERIPH domain (RTC IO incl. GPIO14/15 + the ULP itself)
     * powered during sleep so the ULP can keep bit-banging the IMU bus. */
    if (s_ulp_wakeup) {
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        ret = esp_sleep_enable_ulp_wakeup();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ULP wake enable failed: %s", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "Entering light sleep (touch/BOOT/PWR/timer%s wake)...",
             s_ulp_wakeup ? "/ULP" : "");
    ret = esp_light_sleep_start();   /* blocks until a wake source fires */

    /* Capture the REAL wake cause BEFORE the exit routine tears down the
     * wake sources.  (Returning `ret`/ESP_OK here used to make every wake
     * look like ESP_SLEEP_WAKEUP_UNDEFINED, so the TIMER/ULP branches in the
     * caller never ran and even housekeeping timer wakes fell through to the
     * CNN — a big battery drain.) */
    cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Woke from light sleep (cause=%d)", (int)cause);

    hw_sleep_light_exit();
    return cause;

err:
    hw_sleep_light_exit();
    return cause;
}
