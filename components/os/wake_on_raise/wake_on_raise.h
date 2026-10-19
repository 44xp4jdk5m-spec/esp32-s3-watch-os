/**
 * @file wake_on_raise.h
 * @brief Wake-on-raise: automatically wake the watch when it is lifted from a
 *        flat position.
 *
 * While the display is off and the feature is enabled, an ESP-DL (int8)
 * model classifies the last 1.5 s of QMI8658 IMU data.  When a "raise"
 * gesture is detected the display is woken.  The model lives in PSRAM and is
 * only active while the display is off, so it does not interfere with normal
 * operation (and is freed as soon as the display turns back on).
 *
 * Two run modes:
 *  - CPU detector (`wake_on_raise_start`): samples the IMU at 50 Hz on core 1
 *    and runs the CNN every 100 ms.  Used when light sleep is disabled.
 *  - ULP sampler (`wake_on_raise_ulp_prepare`/`run`): while the main CPU is
 *    in light sleep, the ULP-RISC-V bit-bangs I2C to the QMI8658, keeps a
 *    1.5 s ring buffer in RTC slow memory, runs a fixed-point gate and wakes
 *    the CPU only when the gate fires.  The CPU then runs the CNN on the
 *    buffered window.  Used when light sleep is enabled.
 *
 * The on/off switch is persisted in `g_config.wake_on_raise_enabled` and is
 * exposed in Settings -> Display.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create module resources. Call once during boot (after app_config_init).
 */
esp_err_t wake_on_raise_init(void);

/**
 * @brief Start the wake-on-raise detector (CPU mode).
 *
 * Loads the quantised ESP-DL model and spawns a detection task pinned to
 * core 1 that samples the QMI8658 at 50 Hz.  When a raise is detected the
 * display is woken via app_power_wake().
 *
 * No-op if the detector is already running or the feature is disabled in
 * config.  Called from the power module when the display turns off.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE (disabled), or ESP_ERR_NO_MEM.
 */
esp_err_t wake_on_raise_start(void);

/**
 * @brief Stop the detector and release the model.
 *
 * Safe to call from any task.  When called from a task other than the
 * detector itself this blocks briefly (bounded) until the model is freed.
 * Called automatically whenever the display wakes.  Also stops the ULP
 * sampler if it is active.
 */
void wake_on_raise_stop(void);

/**
 * @brief True while the detection task is running (display off + enabled).
 */
bool wake_on_raise_is_running(void);

/**
 * @brief Whether the feature is enabled in the persisted config.
 */
bool wake_on_raise_is_enabled(void);

/* ── ULP-RISC-V sampler mode ────────────────────────────────────────── */

/**
 * @brief Prepare ULP mode for a display-off sleep cycle.
 *
 * Arms the ULP-RISC-V sampler for ANY light sleep: it always polls the FT3168
 * touch controller over I2C during sleep (the touch INT line is unreliable on
 * this board), and additionally does the IMU raise gate when wake-on-raise is
 * enabled.  Loads the ESP-DL model (only needed for the raise gate) and flags
 * the hw layer to keep the IMU on + add the ULP wake source.  Does NOT start
 * the ULP yet — call wake_on_raise_ulp_run() right before entering light sleep.
 *
 * @return ESP_OK on success (also when wake-on-raise is disabled).
 */
esp_err_t wake_on_raise_ulp_prepare(void);

/**
 * @brief Load + run the ULP sampler (bit-banged IMU + gate).
 *
 * Copies the ULP binary into RTC slow memory and starts the coprocessor.
 * From this call until light sleep is entered, the main CPU must NOT touch
 * the I2C bus (GPIO14/15 are handed to the ULP for bit-banging).
 *
 * @return ESP_OK on success.
 */
esp_err_t wake_on_raise_ulp_run(void);

/**
 * @brief Halt the ULP and restore the I2C pins for the main CPU.
 *
 * Call immediately after waking from light sleep, BEFORE any main-CPU I2C
 * transaction.  Safe to call when the ULP is not running.
 */
void wake_on_raise_ulp_halt(void);

/**
 * @brief Run the CNN on the ULP's ring buffer (after a ULP wake).
 *
 * The ULP must already be halted.  Reads the 1.5 s ring (raw int16 counts),
 * converts to the CNN's normalised input and classifies.
 *
 * @param[out] raise     true if the buffered window is a raise.
 * @param[out] prob_out  optional — receives the raw CNN probability (0..1).
 * @return ESP_OK, or ESP_ERR_INVALID_STATE if the ring is not full yet.
 */
esp_err_t wake_on_raise_ulp_handle_wake(bool *raise, float *prob_out);

/**
 * @brief Number of IMU samples the ULP took during the current sleep run.
 *        Meaningful only while ULP mode is armed; 0 otherwise.
 */
uint32_t wake_on_raise_ulp_sample_count(void);

/**
 * @brief Disarm + stop ULP mode and release the model.
 */
void wake_on_raise_ulp_stop(void);

/**
 * @brief True while ULP mode is armed for this display-off cycle.
 */
bool wake_on_raise_ulp_is_armed(void);

/**
 * @brief True while the ULP coprocessor is currently running.
 */
bool wake_on_raise_ulp_is_running(void);

#ifdef __cplusplus
}
#endif
