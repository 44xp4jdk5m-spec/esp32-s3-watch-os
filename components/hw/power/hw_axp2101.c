/**
 * @file hw_axp2101.c
 * @brief AXP2101 PMIC driver implementation.
 *
 * Power Rail Mapping (ESP32-S3-Touch-AMOLED-2.06):
 *   DC1   = 3.3V — Main system VDD (always on)
 *   ALDO1 = 3.3V — Peripheral VDD
 *   BLDO1 = 3.3V — OLED/Display VDD
 *
 * Disabled: DC2-5, ALDO2-4, BLDO2, CPUSLDO, DLDO1-2
 */

#include "hw_axp2101.h"
#include "hw_config.h"
#include "hw_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "AXP2101";

static i2c_master_dev_handle_t s_dev = NULL;
static axp2101_event_cb_t     s_event_cb = NULL;
static void                  *s_event_user_data = NULL;
static uint8_t                s_last_irq2 = 0;
static bool                   s_initialized = false;

/* ── Internal helpers ──────────────────────────────────────────────────── */
static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return hw_i2c_read_byte(s_dev, reg, val);
}
static esp_err_t wr(uint8_t reg, uint8_t val)
{
    return hw_i2c_write_byte(s_dev, reg, val);
}

/* ── Voltage → register helpers ────────────────────────────────────────── */
static uint8_t dc1_mv_to_reg(uint16_t mv) {
    if (mv < 1500) mv = 1500;
    if (mv > 3400) mv = 3400;
    return (mv - 1500) / 100;
}
static uint8_t ldo_mv_to_reg(uint16_t mv) {
    if (mv < 500) mv = 500;
    if (mv > 3500) mv = 3500;
    return (mv - 500) / 100;
}

/* ── Init ──────────────────────────────────────────────────────────────── */
esp_err_t hw_axp2101_init(void)
{
    if (s_initialized) return ESP_OK;
    ESP_LOGI(TAG, "Initializing AXP2101 PMIC...");

    HW_CHECK(hw_i2c_add_device(HW_AXP2101_I2C_ADDR, &s_dev));

    uint8_t chip_id;
    HW_CHECK(rd(AXP2101_REG_IC_TYPE, &chip_id));
    ESP_LOGI(TAG, "Chip ID: 0x%02X %s", chip_id,
             (chip_id == 0x4A) ? "(OK)" : "(UNEXPECTED!)");

    /* ── Power rails: configure voltage, ensure DC1 stays ON ────────── */
    /*   NEVER disable DC1 — it powers the ESP32 itself!                 */
    /*   Read current state before modifying.                            */

    /* DC1 = 3.3V main system (set voltage, keep ON) */
    wr(AXP2101_REG_DC_VOL0_CTRL, dc1_mv_to_reg(HW_AXP2101_DC1_MV));
    /* Ensure DC1 is enabled (bit0=1 in DC_ONOFF_DVM_CTRL) */
    uint8_t dc_ctrl;
    rd(AXP2101_REG_DC_ONOFF_DVM_CTRL, &dc_ctrl);
    dc_ctrl |= 0x01;  /* Set DC1_EN bit, keep others unchanged */
    wr(AXP2101_REG_DC_ONOFF_DVM_CTRL, dc_ctrl);

    /* ALDO1 = 3.3V peripherals */
    wr(AXP2101_REG_LDO_VOL0_CTRL, ldo_mv_to_reg(3300));
    uint8_t ldo0;
    rd(AXP2101_REG_LDO_ONOFF_CTRL0, &ldo0);
    ldo0 |= 0x01;  /* ALDO1 ON, keep others */
    wr(AXP2101_REG_LDO_ONOFF_CTRL0, ldo0);

    /* BLDO1 = 3.3V display */
    wr(AXP2101_REG_LDO_VOL4_CTRL, ldo_mv_to_reg(3300));
    rd(AXP2101_REG_LDO_ONOFF_CTRL0, &ldo0);
    ldo0 |= 0x10;  /* BLDO1 ON, keep others */
    wr(AXP2101_REG_LDO_ONOFF_CTRL0, ldo0);

    ESP_LOGI(TAG, "Rails: DC1=3.3V ALDO1=3.3V BLDO1=3.3V (ensured ON)");

    /* ── System voltage minimum ──────────────────────────────────────── */
    wr(AXP2101_REG_MIN_SYS_VOL_CTRL, 0x05);      /* ~3.0V VSYS min */

    /* ── Charging config ──────────────────────────────────────────────── */
    wr(AXP2101_REG_IPRECHG_SET, 0x02);            /* 50mA precharge */
    wr(AXP2101_REG_ICC_CHG_SET, (HW_AXP2101_CHG_CUR_MA / 25) - 1); /* 400mA */
    wr(AXP2101_REG_ITERM_CHG_SET, 0x01);          /* 25mA termination */
    wr(AXP2101_REG_CV_CHG_VOL_SET, 0x02);         /* 4.2V target */
    wr(AXP2101_REG_CHG_TIMEOUT_SET, 0x80 | (0x02 << 4)); /* 10h timeout */
    wr(AXP2101_REG_BAT_DET_CTRL, 0x20);           /* Bat detect on */

    /* ── ADC: VBUS, VSYS, TEMP, BAT_V ────────────────────────────────── */
    /* Keep the original 0x78 channel config.  Changing it (0x7D) disturbed
     * the AXP2101's internal fuel-gauge SOC, making the battery % read
     * wrong (~68%% on a full cell). */
    wr(AXP2101_REG_ADC_CHANNEL_CTRL, 0x78);

    /* ── Fuel gauge ──────────────────────────────────────────────────── */
    wr(AXP2101_REG_FUEL_GAUGE_CTRL, 0x01);

    /* ── IRQ: enable PKEY + VBUS + BAT events ────────────────────────── */
    wr(AXP2101_REG_INTEN1, 0x00);
    wr(AXP2101_REG_INTEN2,
        AXP2101_IRQ_PKEY_SHORT | AXP2101_IRQ_PKEY_LONG |
        AXP2101_IRQ_VBUS_INSERT | AXP2101_IRQ_VBUS_REMOVE |
        AXP2101_IRQ_BAT_INSERT | AXP2101_IRQ_BAT_REMOVE);
    wr(AXP2101_REG_INTEN3, 0x00);

    /* Clear pending IRQs */
    rd(AXP2101_REG_INTSTS2, &s_last_irq2);

    /* ── PWR button timing ───────────────────────────────────────────── */
    /* Register 0x27 layout (AXP2101): [5:4]=IRQ long-press time,
     * [3:2]=PEK OFF level, [1:0]=PEK ON level.  XPowersLib enums:
     *   IRQ level: 0=1s 1=1.5s 2=2s 3=2.5s   → 0x95 = 10 = 2s
     *   OFF level: 0=4s 1=6s 2=8s 3=10s      → kept 01 = 6s
     *   ON  level: 0=128ms 1=512ms 2=1s 3=2s → kept 01 = 512ms
     * So the PKEY long-press IRQ (which triggers hw_shutdown → PMIC
     * power-off) fires after holding PWR for 2 seconds. */
    wr(AXP2101_REG_IRQ_OFF_ON_LEVEL, 0x95);       /* 2s long-press threshold */
    /* Only enable software shutdown (bit1). Disable hardware 6 s shutdown
     * (bit0) because the AXP2101's HW shutdown auto-restarts when PEK
     * goes HIGH — i.e. when the user releases the button. Software
     * shutdown via I2C register latches off until next button press. */
    wr(AXP2101_REG_PWROFF_EN, 0x02);              /* SW shutdown only, no HW 6s */

    s_initialized = true;
    ESP_LOGI(TAG, "AXP2101 ready");
    hw_axp2101_dump_status();
    return ESP_OK;
}

esp_err_t hw_axp2101_deinit(void)
{
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    s_initialized = false;
    s_event_cb = NULL;
    return ESP_OK;
}

/* ── Battery / Power data ──────────────────────────────────────────────── */
esp_err_t hw_axp2101_get_battery_data(axp2101_battery_data_t *data)
{
    if (!s_initialized || !data) return ESP_ERR_INVALID_STATE;
    memset(data, 0, sizeof(*data));
    data->battery_percent = -1;

    uint8_t s1, s2;
    rd(AXP2101_REG_STATUS1, &s1);
    rd(AXP2101_REG_STATUS2, &s2);

    data->is_vbus_present    = (s1 & AXP2101_STATUS_VBUS_GOOD) != 0;
    data->is_battery_present = (s1 & AXP2101_STATUS_BAT_PRESENT) != 0;
    /* REG 0x01 bits[2:0] = charging status (AXP2101 datasheet):
     * 000 tri / 001 pre / 010 CC / 011 CV / 100 done → charging/plugged;
     * 101 not charging.  (Bits 5:3 are the battery-CURRENT-DIRECTION field,
     * so reading them made "charging" come out inverted.) */
    uint8_t chg = s2 & 0x07;
    data->is_charging        = (chg <= 4);
    data->charger_status     = (axp2101_charger_status_t)chg;

    /* VBUS voltage: ADC_DATA 0x38(H6)+0x39(L8) = 14-bit, 1.7mV/LSB */
    uint8_t vh, vl;
    rd(0x38, &vh); rd(0x39, &vl);
    data->vbus_voltage = (((uint16_t)(vh & 0x3F) << 8) | vl) * 1.7f;

    /* Battery voltage: 0x34(H5)+0x35(L8), 1.1mV/LSB */
    rd(0x34, &vh); rd(0x35, &vl);
    data->battery_voltage = (((uint16_t)(vh & 0x1F) << 8) | vl) * 1.1f;

    /* System voltage: ADC_DATA 0x3A(H6)+0x3B(L8) = 14-bit, 1.7mV/LSB */
    rd(0x3A, &vh); rd(0x3B, &vl);
    data->system_voltage = (((uint16_t)(vh & 0x3F) << 8) | vl) * 1.7f;

    /* IC die temperature: ADC_DATA 0x3C(H6)+0x3D(L8) = 14-bit.
     * (0x5A/0x5B are the JIETA thermal-threshold CONFIG registers and
     * always read back the same constant — that's why the old reading
     * was stuck at 25.2 °C.  These are the die-temperature ADC regs.)
     * Formula (XPowersLib): T(°C) = 22.0 + (7274 - raw) / 20.0 */
    rd(0x3C, &vh); rd(0x3D, &vl);
    uint16_t traw = (((uint16_t)(vh & 0x3F) << 8) | vl) & 0x3FFF;
    data->temperature = (traw <= 16383) ? (22.0f + (7274.0f - (float)traw) / 20.0f)
                                        : 0.0f;

    /* Battery percentage — from the AXP2101 fuel-gauge SOC engine.
     * (Verified by the user to be accurate to ~±3%, incl. while charging.) */
    uint8_t pct;
    if (rd(AXP2101_REG_BAT_PERCENT_DATA, &pct) == ESP_OK) {
        data->battery_percent = (pct & 0x7F);
    }
    return ESP_OK;
}

esp_err_t hw_axp2101_get_battery_voltage(float *mv)
{
    axp2101_battery_data_t d;
    HW_CHECK(hw_axp2101_get_battery_data(&d));
    *mv = d.battery_voltage;
    return ESP_OK;
}

esp_err_t hw_axp2101_get_battery_percent(int *pct)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    uint8_t v;
    HW_CHECK(rd(AXP2101_REG_BAT_PERCENT_DATA, &v));
    *pct = (v & 0x7F);
    return ESP_OK;
}

esp_err_t hw_axp2101_get_temperature(float *c)
{
    axp2101_battery_data_t d;
    HW_CHECK(hw_axp2101_get_battery_data(&d));
    *c = d.temperature;
    return ESP_OK;
}

esp_err_t hw_axp2101_is_vbus_present(bool *p)
{
    uint8_t v;
    HW_CHECK(rd(AXP2101_REG_STATUS1, &v));
    *p = (v & AXP2101_STATUS_VBUS_GOOD) != 0;
    return ESP_OK;
}

esp_err_t hw_axp2101_is_charging(bool *p)
{
    uint8_t v;
    HW_CHECK(rd(AXP2101_REG_STATUS2, &v));
    /* REG 0x01 bits[2:0] = charging status (0..4 charge/done, 5 = not). */
    uint8_t c = v & 0x07;
    *p = (c <= 4);
    return ESP_OK;
}

/* ── Sleep / Shutdown ──────────────────────────────────────────────────── */
esp_err_t hw_axp2101_enter_sleep(void)
{
    ESP_LOGI(TAG, "PMIC → sleep mode");
    return wr(AXP2101_REG_SLEEP_WAKEUP_CTRL, 0x01);
}

esp_err_t hw_axp2101_exit_sleep(void)
{
    ESP_LOGI(TAG, "PMIC → wake");
    return wr(AXP2101_REG_SLEEP_WAKEUP_CTRL, 0x00);
}

void hw_axp2101_shutdown(void)
{
    ESP_LOGW(TAG, "SHUTDOWN via PMIC");

    /* Diagnose the power-off config first.  On this Waveshare board the
     * soft-PWROFF path (REG10 bit0) is gated by REG21 bit1 ("Software
     * configuration as POWEROFF Source"), which is read-only and POR=0
     * (disabled), so REG10 bit0 alone does NOT cut power.  The hardware
     * POK power-off (6 s hold) can't be emulated either — GPIO10 sits on the
     * opposite side of the open PWR button from PWRON (schematic: PWRON ->
     * 510R -> Key3 -> GPIO10), so driving it never grounds PWRON.  The only
     * software-triggerable true power-off left is AXP sleep mode + DC1 drop. */
    uint8_t pwroff_status = 0, pwroff_en = 0, common = 0;
    if (rd(AXP2101_REG_PWROFF_STATUS, &pwroff_status) != ESP_OK)
        ESP_LOGE(TAG, "read REG21 (PWROFF_STATUS) FAILED");
    if (rd(AXP2101_REG_PWROFF_EN, &pwroff_en) != ESP_OK)
        ESP_LOGE(TAG, "read REG22 (PWROFF_EN) FAILED");
    if (rd(AXP2101_REG_COMMON_CONFIG, &common) != ESP_OK)
        ESP_LOGE(TAG, "read REG10 (COMMON_CONFIG) FAILED");
    ESP_LOGW(TAG, "PWROFF_STATUS=0x%02X (soft-pwroff src=%d)  PWROFF_EN=0x%02X (POK-off=%d)  COMMON=0x%02X",
             pwroff_status, (pwroff_status >> 1) & 1,
             pwroff_en, (pwroff_en >> 1) & 1, common);

    /* Attempt 1: soft power-off with a CLEAN REG10 first.
     * The PMIC holds a stale 0x80 (reserved bit7 from an old firmware's
     * wr(0x80)); restore the POR default (0x30) before arming bit0. */
    ESP_LOGW(TAG, "Attempt 1: clean soft PWROFF (COMMON 0x%02X -> 0x30 -> 0x31)", common);
    wr(AXP2101_REG_COMMON_CONFIG, 0x30);
    vTaskDelay(pdMS_TO_TICKS(10));
    wr(AXP2101_REG_COMMON_CONFIG, 0x31);   /* bit0 = Soft PWROFF */
    vTaskDelay(pdMS_TO_TICKS(200));
    if (rd(AXP2101_REG_COMMON_CONFIG, &common) == ESP_OK) {
        ESP_LOGW(TAG, "Still alive after soft PWROFF (COMMON=0x%02X) — source disabled in OTP", common);
    } else {
        ESP_LOGW(TAG, "I2C gone — PMIC powered off!");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); } /* unreachable if dead */
    }

    /* Attempt 2: AXP sleep mode = true power-off; a PWR press wakes via IRQ.
     * Enter sleep FIRST (the PMU records REG80/90/91 with DC1 still ON, so
     * wakeup restores it and the ESP32 reboots), then drop the rails — DC1
     * last so the ESP32 truly loses power. */
    ESP_LOGW(TAG, "Attempt 2: AXP sleep + DC1 power-down ...");
    wr(AXP2101_REG_SLEEP_WAKEUP_CTRL, 0x11);   /* bit4 IRQ-low wakeup, bit0 sleep */
    vTaskDelay(pdMS_TO_TICKS(100));
    wr(AXP2101_REG_LDO_ONOFF_CTRL0, 0x00);     /* ALDO1/BLDO1 off */
    wr(AXP2101_REG_DC_ONOFF_DVM_CTRL, 0x00);   /* DC1 off -> ESP32 loses power */
    ESP_LOGW(TAG, "Rails powered down — awaiting PWR press to wake");
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

/* ── Charging control (thermal protection) ────────────────────────────── */
esp_err_t hw_axp2101_charging_disable(void)
{
    ESP_LOGW(TAG, "Charging DISABLED");
    /* AXP2101_REG_CHG_GAUGE_WDT (0x18) bit 1 = cell-battery charger enable
     * (XPowersLib enableCellbatteryCharge).  Bit 7 is NOT the charger enable
     * — clearing it (as the old code did) was a no-op, so the 80%% charge
     * limit and thermal protection never actually stopped charging. */
    uint8_t val;
    esp_err_t ret = rd(AXP2101_REG_CHG_GAUGE_WDT, &val);
    if (ret != ESP_OK) return ret;
    val &= ~0x02;  /* clear cell-charger-enable bit */
    return wr(AXP2101_REG_CHG_GAUGE_WDT, val);
}

esp_err_t hw_axp2101_charging_enable(void)
{
    ESP_LOGI(TAG, "Charging ENABLED");
    uint8_t val;
    esp_err_t ret = rd(AXP2101_REG_CHG_GAUGE_WDT, &val);
    if (ret != ESP_OK) return ret;
    val |= 0x02;   /* set cell-charger-enable bit */
    return wr(AXP2101_REG_CHG_GAUGE_WDT, val);
}

esp_err_t hw_axp2101_set_charge_current(uint16_t ma)
{
    /* AXP2101_REG_ICC_CHG_SET (0x62): charge current = (N + 1) × 25 mA,
     * N in bits [4:0] (0 → 25 mA ... 31 → 800 mA).  Bits [5:7] hold other
     * configuration — read-modify-write to preserve them. */
    if (ma < 25)  ma = 25;
    if (ma > 800) ma = 800;
    uint8_t n  = (uint8_t)((ma / 25) - 1);
    uint8_t cur;
    esp_err_t ret = rd(AXP2101_REG_ICC_CHG_SET, &cur);
    if (ret != ESP_OK) return ret;
    cur = (cur & 0xE0) | (n & 0x1F);
    return wr(AXP2101_REG_ICC_CHG_SET, cur);
}

/* ── Event callbacks ───────────────────────────────────────────────────── */
esp_err_t hw_axp2101_register_event_callback(axp2101_event_cb_t cb, void *user_data)
{
    s_event_cb = cb;
    s_event_user_data = user_data;
    return ESP_OK;
}

esp_err_t hw_axp2101_poll_events(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    uint8_t irq2;
    rd(AXP2101_REG_INTSTS2, &irq2);

    if (irq2 == s_last_irq2) return ESP_ERR_NOT_FOUND;

    uint8_t new_irqs = irq2 & ~s_last_irq2;
    s_last_irq2 = irq2;

    if (s_event_cb && new_irqs) {
        if (new_irqs & AXP2101_IRQ_PKEY_SHORT)  s_event_cb(AXP2101_EVENT_PWR_SHORT_PRESS, s_event_user_data);
        if (new_irqs & AXP2101_IRQ_PKEY_LONG)   s_event_cb(AXP2101_EVENT_PWR_LONG_PRESS, s_event_user_data);
        if (new_irqs & AXP2101_IRQ_VBUS_INSERT) s_event_cb(AXP2101_EVENT_VBUS_INSERT, s_event_user_data);
        if (new_irqs & AXP2101_IRQ_VBUS_REMOVE) s_event_cb(AXP2101_EVENT_VBUS_REMOVE, s_event_user_data);
        if (new_irqs & AXP2101_IRQ_BAT_INSERT)  s_event_cb(AXP2101_EVENT_BAT_INSERT, s_event_user_data);
        if (new_irqs & AXP2101_IRQ_BAT_REMOVE)  s_event_cb(AXP2101_EVENT_BAT_REMOVE, s_event_user_data);
    }
    return ESP_OK;
}

/* ── Debug ─────────────────────────────────────────────────────────────── */
void hw_axp2101_dump_status(void)
{
    axp2101_battery_data_t d;
    hw_axp2101_get_battery_data(&d);
    ESP_LOGI(TAG, "=== AXP2101 Status ===");
    ESP_LOGI(TAG, "  VBUS=%s (%.0fmV)  Bat=%s (%.0fmV, %d%%)",
             d.is_vbus_present ? "YES" : "NO", d.vbus_voltage,
             d.is_battery_present ? "YES" : "NO", d.battery_voltage, d.battery_percent);
    ESP_LOGI(TAG, "  Sys=%.0fmV  Temp=%.1fC  Charging=%s", d.system_voltage, d.temperature,
             d.is_charging ? "YES" : "NO");
}
