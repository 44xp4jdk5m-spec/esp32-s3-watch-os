/**
 * @file hw_axp2101.h
 * @brief AXP2101 PMIC driver — power management, battery, charging, sleep.
 *
 * I2C address: 0x34
 * PWR button (GPIO10) routes through AXP2101 PEK pin:
 *   - Short press → IRQ fires PKEY_SHORT
 *   - Long press (~1.5s) → IRQ fires PKEY_LONG
 *   - Very long press (6s) → hardware shutdown (non-configurable)
 */

#pragma once

#include "hw_common.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── AXP2101 Register Map ──────────────────────────────────────────────── */
#define AXP2101_REG_STATUS1             0x00
#define AXP2101_REG_STATUS2             0x01
#define AXP2101_REG_IC_TYPE             0x03
#define AXP2101_REG_COMMON_CONFIG       0x10
#define AXP2101_REG_MIN_SYS_VOL_CTRL    0x14
#define AXP2101_REG_INPUT_VOL_LIMIT     0x15
#define AXP2101_REG_INPUT_CUR_LIMIT     0x16
#define AXP2101_REG_CHG_GAUGE_WDT       0x18
#define AXP2101_REG_WDT_CTRL            0x19
#define AXP2101_REG_LOW_BAT_WARN        0x1A
#define AXP2101_REG_PWROFF_STATUS       0x21    /* which sources can power off (bit1 = software) */
#define AXP2101_REG_PWROFF_EN           0x22
#define AXP2101_REG_VOFF_SET            0x24
#define AXP2101_REG_SLEEP_WAKEUP_CTRL   0x26
#define AXP2101_REG_IRQ_OFF_ON_LEVEL    0x27
#define AXP2101_REG_ADC_CHANNEL_CTRL    0x30
#define AXP2101_REG_INTEN1              0x40
#define AXP2101_REG_INTEN2              0x41
#define AXP2101_REG_INTEN3              0x42
#define AXP2101_REG_INTSTS1             0x48
#define AXP2101_REG_INTSTS2             0x49
#define AXP2101_REG_INTSTS3             0x4A
#define AXP2101_REG_IPRECHG_SET         0x61
#define AXP2101_REG_ICC_CHG_SET         0x62
#define AXP2101_REG_ITERM_CHG_SET       0x63
#define AXP2101_REG_CV_CHG_VOL_SET      0x64
#define AXP2101_REG_CHG_TIMEOUT_SET     0x67
#define AXP2101_REG_BAT_DET_CTRL        0x68
#define AXP2101_REG_DC_ONOFF_DVM_CTRL   0x80
#define AXP2101_REG_DC_VOL0_CTRL        0x82    /* DC1 */
#define AXP2101_REG_DC_VOL1_CTRL        0x83    /* DC2 */
#define AXP2101_REG_DC_VOL2_CTRL        0x84    /* DC3 */
#define AXP2101_REG_DC_VOL3_CTRL        0x85    /* DC4 */
#define AXP2101_REG_DC_VOL4_CTRL        0x86    /* DC5 */
#define AXP2101_REG_LDO_ONOFF_CTRL0     0x90
#define AXP2101_REG_LDO_ONOFF_CTRL1     0x91
#define AXP2101_REG_LDO_VOL0_CTRL       0x92    /* ALDO1 */
#define AXP2101_REG_LDO_VOL4_CTRL       0x96    /* BLDO1 */
#define AXP2101_REG_FUEL_GAUGE_CTRL     0xA2
#define AXP2101_REG_BAT_PERCENT_DATA    0xA4

/* ── IRQ bit masks (in INTSTS2) ───────────────────────────────────────── */
#define AXP2101_IRQ_VBUS_INSERT      (1 << 0)
#define AXP2101_IRQ_VBUS_REMOVE      (1 << 1)
#define AXP2101_IRQ_BAT_INSERT       (1 << 2)
#define AXP2101_IRQ_BAT_REMOVE       (1 << 3)
#define AXP2101_IRQ_PKEY_SHORT       (1 << 4)
#define AXP2101_IRQ_PKEY_LONG        (1 << 5)

/* ── Status masks ─────────────────────────────────────────────────────── */
#define AXP2101_STATUS_VBUS_GOOD     (1 << 5)
#define AXP2101_STATUS_BAT_PRESENT   (1 << 3)
#define AXP2101_STATUS_CHARGING      (1 << 2)

/* ── Charger status ───────────────────────────────────────────────────── */
typedef enum {
    AXP2101_CHG_NOT_CHARGING = 0,
    AXP2101_CHG_TRICKLE,
    AXP2101_CHG_PRE,
    AXP2101_CHG_FAST,
    AXP2101_CHG_DONE,
    AXP2101_CHG_STOP,
} axp2101_charger_status_t;

/* ── Events ───────────────────────────────────────────────────────────── */
typedef enum {
    AXP2101_EVENT_BATTERY_UPDATE,
    AXP2101_EVENT_CHARGING_START,
    AXP2101_EVENT_CHARGING_DONE,
    AXP2101_EVENT_VBUS_INSERT,
    AXP2101_EVENT_VBUS_REMOVE,
    AXP2101_EVENT_PWR_SHORT_PRESS,
    AXP2101_EVENT_PWR_LONG_PRESS,
    AXP2101_EVENT_LOW_BATTERY,
    AXP2101_EVENT_BAT_INSERT,
    AXP2101_EVENT_BAT_REMOVE,
} axp2101_event_t;

typedef void (*axp2101_event_cb_t)(axp2101_event_t event, void *user_data);

/* ── Battery data ─────────────────────────────────────────────────────── */
typedef struct {
    float battery_voltage;      /**< mV */
    float vbus_voltage;         /**< mV, 0 if no USB */
    float system_voltage;       /**< mV */
    float temperature;          /**< °C */
    int   battery_percent;      /**< 0-100, -1 unknown */
    bool  is_charging;
    bool  is_vbus_present;
    bool  is_battery_present;
    axp2101_charger_status_t charger_status;
} axp2101_battery_data_t;

/* ── API ──────────────────────────────────────────────────────────────── */
esp_err_t hw_axp2101_init(void);
esp_err_t hw_axp2101_deinit(void);

esp_err_t hw_axp2101_get_battery_data(axp2101_battery_data_t *data);
esp_err_t hw_axp2101_get_battery_voltage(float *voltage_mv);
esp_err_t hw_axp2101_get_battery_percent(int *percent);
esp_err_t hw_axp2101_get_temperature(float *temp_c);
esp_err_t hw_axp2101_is_vbus_present(bool *present);
esp_err_t hw_axp2101_is_charging(bool *charging);

esp_err_t hw_axp2101_enter_sleep(void);
esp_err_t hw_axp2101_exit_sleep(void);
void     hw_axp2101_shutdown(void);

/** @brief Disable battery charging (thermal protection). */
esp_err_t hw_axp2101_charging_disable(void);

/** @brief Re-enable battery charging after thermal protection. */
esp_err_t hw_axp2101_charging_enable(void);

/** @brief Set the battery charge current in mA (25 mA steps, 25–800 mA). */
esp_err_t hw_axp2101_set_charge_current(uint16_t ma);

esp_err_t hw_axp2101_register_event_callback(axp2101_event_cb_t cb, void *user_data);
esp_err_t hw_axp2101_poll_events(void);
void     hw_axp2101_dump_status(void);

#ifdef __cplusplus
}
#endif
