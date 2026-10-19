/**
 * @file hw_sleep.h
 * @brief Light-sleep support for the ESP32-S3 watch.
 *
 * The ESP32-S3 halts the main CPU while retaining RAM (light sleep), which
 * cuts power dramatically versus the old "CPU always on, keep polling"
 * standby.  Wake sources used here:
 *   - Touch INT   (GPIO38, active LOW)  -> FT3168 touch panel
 *   - BOOT button (GPIO0,  active LOW)
 *   - PWR button  (GPIO10, active HIGH) -> AXP2101 PEK
 *   - Housekeeping timer                 -> periodic PMIC poll, then re-sleep
 *
 * Only `gpio_wakeup_enable()` (light-sleep GPIO wake, works on ANY GPIO,
 * including non-RTC GPIO38) is used — NOT ext0/ext1, so no RTC-IO de-init
 * is required after wake.
 */

#pragma once

#include "hw_common.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Wake pin config (matches hw_config.h) ──────────────────────────── */
#define HW_SLEEP_WAKE_TOUCH_GPIO    GPIO_NUM_38   /**< FT3168 INT, active low */
#define HW_SLEEP_WAKE_BOOT_GPIO     GPIO_NUM_0    /**< BOOT button, active low */
#define HW_SLEEP_WAKE_PWR_GPIO      GPIO_NUM_10   /**< PWR button, active high */

/* Housekeeping timer wake period (us).  The system wakes briefly to poll
 * the PMIC (charging / VBUS / battery events) then re-enters light sleep.
 * Set to 0 to disable the periodic wake entirely. */
//#define HW_SLEEP_HOUSEKEEP_PERIOD_US (60 * 1000 * 1000)
#define HW_SLEEP_HOUSEKEEP_PERIOD_US 0

/**
 * @brief Set the housekeeping timer wake period for the NEXT light-sleep
 *        entry (us); 0 disables it.  The power manager recomputes this
 *        before every sleep so timer wakes only happen when real work is
 *        due (e.g. the top of the hour for battery logging), not on a
 *        constant schedule.
 */
void hw_sleep_set_housekeep_period_us(uint64_t us);

/**
 * @brief Enter light sleep with the configured wake sources.
 *
 * Blocks until the chip wakes (any of the wake sources).  On return the
 * wake sources are cleaned up automatically (see hw_sleep_light_exit).
 *
 * @return The wake-up cause of the last light-sleep exit
 *         (esp_sleep_get_wakeup_cause), or ESP_SLEEP_WAKEUP_UNDEFINED if
 *         sleep was rejected/aborted before actually sleeping.
 */
esp_sleep_wakeup_cause_t hw_sleep_light_enter(void);

/**
 * @brief Restore GPIO/wake configuration after waking from light sleep.
 *
 * Disables the GPIO + timer wake sources and restores GPIO0/GPIO10/GPIO38
 * to their normal input configuration so button/touch polling works again.
 * Called automatically at the end of hw_sleep_light_enter(); you normally
 * do not need to call it yourself.
 */
void hw_sleep_light_exit(void);

/**
 * @brief Wake-up cause of the last light-sleep exit (passthrough).
 */
static inline esp_sleep_wakeup_cause_t hw_sleep_get_wakeup_cause(void)
{
    return esp_sleep_get_wakeup_cause();
}

/**
 * @brief Keep the IMU powered in standby (ULP wake-on-raise sampler).
 *
 * When the ULP-RISC-V is sampling the QMI8658 during light sleep,
 * hw_enter_standby() must NOT put the IMU to sleep.  Set before entering
 * the ULP sleep mode, clear after.
 */
void hw_sleep_set_keep_imu(bool keep);
bool hw_sleep_get_keep_imu(void);

/**
 * @brief Enable the ULP coprocessor as a light-sleep wake source.
 *
 * Must be set (with the ULP program loaded + running) before the ULP's
 * fixed-point gate can wake the main CPU.  Cleared on exit.
 */
void hw_sleep_set_ulp_wakeup(bool enable);

#ifdef __cplusplus
}
#endif
