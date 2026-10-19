/** @file hw_button.h — Physical button driver */
#pragma once
#include "hw_common.h"
#include "hw_axp2101.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HW_BTN_BOOT = 0, HW_BTN_PWR = 1 } hw_button_id_t;
typedef enum { HW_BTN_EVENT_CLICK, HW_BTN_EVENT_LONG_PRESS, HW_BTN_EVENT_VERY_LONG } hw_button_event_t;
typedef void (*hw_button_cb_t)(hw_button_id_t btn, hw_button_event_t evt, void *user_data);

esp_err_t hw_button_init(void);
esp_err_t hw_button_deinit(void);
esp_err_t hw_button_register_callback(hw_button_cb_t cb, void *user_data);
bool      hw_button_is_pressed(hw_button_id_t btn);

/**
 * @brief Forward AXP2101 PWR-button events to the button subsystem.
 *        Call this from your AXP2101 event callback (the sole owner
 *        of the PMIC callback slot) for PKEY_SHORT / PKEY_LONG events.
 */
void hw_button_handle_axp_event(axp2101_event_t event);

#ifdef __cplusplus
}
#endif
