/** @file hw_button.c */
#include "hw_button.h"
#include "hw_config.h"
#include "hw_axp2101.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "HW_BTN";
static hw_button_cb_t s_cb = NULL;
static void *s_udata = NULL;
static TimerHandle_t s_timer = NULL;
static bool s_boot_down = false;
static bool s_pwr_down = false;
static bool s_pwr_long_sent = false;   /* LONG_PRESS already fired at 2 s */
static TickType_t s_boot_tick = 0;
static TickType_t s_pwr_tick = 0;

static void boot_timer_cb(TimerHandle_t xTimer)
{
    /* ── BOOT button (GPIO0, active LOW) ── */
    bool boot_now = (gpio_get_level(HW_BTN_BOOT_GPIO) == 0);
    if (boot_now && !s_boot_down) {
        s_boot_down = true; s_boot_tick = xTaskGetTickCount();
    } else if (!boot_now && s_boot_down) {
        s_boot_down = false;
        uint32_t ms = (xTaskGetTickCount() - s_boot_tick) * portTICK_PERIOD_MS;
        if (s_cb) {
            if (ms > 1000) s_cb(HW_BTN_BOOT, HW_BTN_EVENT_LONG_PRESS, s_udata);
            else           s_cb(HW_BTN_BOOT, HW_BTN_EVENT_CLICK, s_udata);
        }
    }

    /* ── PWR button (GPIO10, active HIGH via AXP2101 PEK) ──
     * Direct GPIO poll as reliable fallback — the AXP2101 IRQ may
     * use a different register mapping than the driver expects.
     * Long-press threshold 2 s: the LONG_PRESS fires WHILE the button
     * is still held (no release needed), so the shutdown screen appears
     * immediately at the 2 s mark. */
    bool pwr_now = (gpio_get_level(HW_BTN_PWR_GPIO) == 1);
    if (pwr_now && !s_pwr_down) {
        s_pwr_down = true; s_pwr_tick = xTaskGetTickCount(); s_pwr_long_sent = false;
    } else if (pwr_now && s_pwr_down) {
        /* Still held — fire LONG_PRESS once at the 2 s threshold. */
        uint32_t ms = (xTaskGetTickCount() - s_pwr_tick) * portTICK_PERIOD_MS;
        if (!s_pwr_long_sent && ms > 2000) {
            s_pwr_long_sent = true;
            if (s_cb) s_cb(HW_BTN_PWR, HW_BTN_EVENT_LONG_PRESS, s_udata);
        }
    } else if (!pwr_now && s_pwr_down) {
        s_pwr_down = false;
        uint32_t ms = (xTaskGetTickCount() - s_pwr_tick) * portTICK_PERIOD_MS;
        if (s_cb && !s_pwr_long_sent) {
            if (ms > 2000) s_cb(HW_BTN_PWR, HW_BTN_EVENT_LONG_PRESS, s_udata);
            else           s_cb(HW_BTN_PWR, HW_BTN_EVENT_CLICK, s_udata);
        }
        s_pwr_long_sent = false;
    }
}

static void axp_evt_cb(axp2101_event_t evt, void *ud)
{
    if (!s_cb) return;
    switch (evt) {
    case AXP2101_EVENT_PWR_SHORT_PRESS:
        s_cb(HW_BTN_PWR, HW_BTN_EVENT_CLICK, s_udata); break;
    case AXP2101_EVENT_PWR_LONG_PRESS:
        s_cb(HW_BTN_PWR, HW_BTN_EVENT_LONG_PRESS, s_udata); break;
    default: break;
    }
}

/* Public entry for the AXP2101 callback owner (app_events.c).
 * DO NOT register this directly — app_events.c owns the PMIC callback. */
void hw_button_handle_axp_event(axp2101_event_t event)
{
    axp_evt_cb(event, NULL);
}

esp_err_t hw_button_init(void)
{
    ESP_LOGI(TAG, "Initializing buttons...");

    /* BOOT button (GPIO0, active low, with internal pull-up) */
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(HW_BTN_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* PWR button (GPIO10) — floating input ONLY.
     * Must NOT have any pull-up or pull-down!
     * GPIO10 is connected to AXP2101's PEK pin. The PMIC handles all
     * button logic internally. If we enable a pull-up here, it will
     * drive PEK HIGH after shutdown and cause the AXP2101 to
     * immediately power back on — defeating the 6s hardware shutdown. */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = BIT64(HW_BTN_PWR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);

    s_timer = xTimerCreate("btn", pdMS_TO_TICKS(30), pdTRUE, NULL, boot_timer_cb);
    xTimerStart(s_timer, 0);

    /* DO NOT register our own AXP2101 callback here!
     * app_events.c owns the single PMIC callback slot and delegates
     * PWR-button events to us via hw_button_handle_axp_event(). */

    ESP_LOGI(TAG, "Buttons ready (BOOT=GPIO0 PWR=AXP2101)");
    return ESP_OK;
}

esp_err_t hw_button_deinit(void)
{
    if (s_timer) { xTimerDelete(s_timer, 0); s_timer = NULL; }
    return ESP_OK;
}

esp_err_t hw_button_register_callback(hw_button_cb_t cb, void *ud)
{
    s_cb = cb; s_udata = ud;
    return ESP_OK;
}

bool hw_button_is_pressed(hw_button_id_t btn)
{
    if (btn == HW_BTN_BOOT) return (gpio_get_level(HW_BTN_BOOT_GPIO) == 0);
    return false; /* PWR state not directly readable */
}
