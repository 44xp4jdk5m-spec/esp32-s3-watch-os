/**
 * @file app_power.h
 * @brief Power management: inactivity dim/sleep, brightness control.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Smoothly transition brightness using a for-loop.
 * @param target  0–100 target brightness
 * @param step_ms milliseconds between each 2% step
 */
void app_power_set_brightness_smooth(uint8_t target, int step_ms);

/**
 * @brief Graceful system reboot.
 *
 * Dims the display to 0 (avoids an abnormal strip on the next boot),
 * saves the config, stops the WiFi file server and BLE/ANCS, unmounts
 * the SD card, then restarts.  Safe to call from an LVGL event callback
 * — the work runs on its own FreeRTOS task.
 */
void app_power_reboot(void);

/**
 * @brief Check inactivity and dim/turn off display as needed.
 *        Call from main loop inside LVGL lock.
 *
 * Rules:
 *   30s no touch → half brightness
 *   +10s (40s)  → display off + standby
 */
void app_power_check_inactivity(void);

/**
 * @brief Wake display from dimmed/off state (called on button press).
 */
void app_power_wake(void);

/**
 * @brief Wake display for automatic events (like charging).
 *        Does NOT reset the inactivity timer, so the display
 *        will dim/off again quickly after the event ends.
 */
void app_power_wake_auto(void);

/**
 * @brief Check if touch is currently pressed (via BSP, not LVGL).
 */
bool app_power_is_touch_pressed(void);

/**
 * @brief Set whether Always-On Display is enabled.
 *        When true, after the idle timeout the display dims into AOD mode
 *        (~12% brightness + ~1Hz panel idle refresh + pixel shift) instead
 *        of turning off.
 * @param enable  true = AOD on, false = normal dim → off behaviour
 */
void app_power_set_aod_enabled(bool enable);

/**
 * @brief Get current AOD setting.
 */
bool app_power_get_aod_enabled(void);

/**
 * @brief Enter the Always-On Display low-power mode: the screen stays on but
 *        dims to ~AOD_BRIGHTNESS, the panel drops to ~1Hz idle refresh
 *        (MIPI DCS Idle Mode ON), and the current watch face gets periodic
 *        pixel shifts for burn-in prevention.  Exit via app_power_wake() /
 *        app_power_exit_aod() (touch, GPIO, or wake-on-raise).
 */
void app_power_enter_aod(void);

/**
 * @brief Leave AOD mode: panel back to full refresh, brightness restored.
 */
void app_power_exit_aod(void);

/**
 * @brief True while the AOD low-power mode is currently active.
 */
bool app_power_get_aod_active(void);

/**
 * @brief Reconcile idle peripheral parking (AOD / Low-Power Mode) with the
 *        current state.  Call once per main-loop iteration (right after
 *        app_power_check_inactivity).  Powers down the audio codecs + SD
 *        card + IMU while idle on the watch face (AOD / Low-Power dim / off)
 *        and restores them the moment the user is active or opens an app.
 */
void app_power_periph_park_tick(void);

/**
 * @brief Force-restore parked peripherals immediately (e.g. before an action
 *        that needs the SD card / audio while the watch is still idle, such
 *        as a screenshot).  Idempotent.
 */
void app_power_periphs_restore(void);

/**
 * @brief True while the non-display peripherals are currently parked.
 */
bool app_power_periphs_parked(void);

/**
 * @brief Turn Low Power Mode on/off and apply all its effects immediately:
 *        kill running animations, cap LVGL render to ~15 FPS, force the dim
 *        timeout to 5 s and brightness to 80% (restored on disable).
 *        Shared by the Settings switch and the control-center button.
 */
void app_power_set_low_power(bool enable);

#ifdef __cplusplus
}
#endif
