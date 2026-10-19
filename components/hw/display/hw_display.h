/**
 * @file hw_display.h
 * @brief Display wrapper around BSP — adds sleep/wake control.
 */
#pragma once

#include "hw_common.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hw_display_init(void);
esp_err_t hw_display_deinit(void);
esp_err_t hw_display_set_brightness(uint8_t percent);
uint8_t  hw_display_get_brightness(void);
esp_err_t hw_display_backlight_on(void);
esp_err_t hw_display_backlight_off(void);
esp_err_t hw_display_sleep(void);
esp_err_t hw_display_wake(void);

/**
 * @brief Pause LVGL rendering (stop timer-driven flushes to the panel).
 *        Call before powering the display down so the LVGL task never races
 *        SPI commands on a busy bus (prevents a bus-deadlock freeze).
 *        Idempotent. Pairs with hw_display_resume_rendering().
 */
esp_err_t hw_display_pause_rendering(void);

/**
 * @brief Resume LVGL rendering after hw_display_pause_rendering().
 */
esp_err_t hw_display_resume_rendering(void);

/**
 * @brief Enter/leave MIPI DCS "Idle Mode" (0x39/0x38) — the AMOLED AOD
 *        low-refresh mode.  When ON, the SH8601 stops full-rate scanning
 *        and self-refreshes the last frame at ~1 Hz, cutting panel power.
 *        Best-effort: logs and returns the error if the panel rejects it.
 * @param idle  true = Idle Mode ON (1Hz), false = Idle Mode OFF (normal)
 */
esp_err_t hw_display_set_idle(bool idle);

/**
 * @brief Change the LVGL display-refresh timer period to cap the render
 *        rate (Low Power Mode: 66 ms ≈ 15 FPS).  Pass 0 to restore the
 *        default (LV_DEF_REFR_PERIOD, 15 ms ≈ 66 FPS).  This build has
 *        LV_USE_PERF_MONITOR on, so the refresh timer runs periodically and
 *        its period genuinely limits how often frames are rendered.
 */
esp_err_t hw_display_set_refr_period_ms(uint32_t ms);

lv_display_t *hw_display_get_lvgl_display(void);
bool     hw_display_lock(int timeout_ms);
void     hw_display_unlock(void);

#ifdef __cplusplus
}
#endif
