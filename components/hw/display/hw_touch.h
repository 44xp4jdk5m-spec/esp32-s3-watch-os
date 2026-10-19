/** @file hw_touch.h — Touch controller wrapper */
#pragma once
#include "hw_common.h"
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t hw_touch_init(void);
esp_err_t hw_touch_sleep(void);
esp_err_t hw_touch_wake(void);
esp_err_t hw_touch_reinit(void);
lv_indev_t *hw_touch_get_indev(void);

/**
 * @brief Direct FT3168 touch-point poll (reg 0x02), independent of LVGL.
 * @return true if the FT3168 reports at least one active touch point.
 */
bool hw_touch_is_touched(void);
#ifdef __cplusplus
}
#endif
