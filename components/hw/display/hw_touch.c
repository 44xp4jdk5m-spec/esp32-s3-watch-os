/** @file hw_touch.c */
#include "hw_touch.h"
#include "hw_i2c.h"
#include "hw_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HW_TOUCH";
static i2c_master_dev_handle_t s_tp_dev = NULL;

static esp_err_t hw_touch_dev_get(void)
{
    if (s_tp_dev) return ESP_OK;
    return hw_i2c_add_device(HW_FT3168_I2C_ADDR, &s_tp_dev);
}

esp_err_t hw_touch_init(void) { ESP_LOGI(TAG, "Touch ready (via BSP)"); return ESP_OK; }
esp_err_t hw_touch_sleep(void) { ESP_LOGI(TAG, "Touch → monitor mode"); return ESP_OK; }
esp_err_t hw_touch_wake(void) { ESP_LOGI(TAG, "Touch → wake"); return ESP_OK; }
lv_indev_t *hw_touch_get_indev(void) { return bsp_display_get_input_dev(); }

/* Direct FT3168 touch-point poll (reg 0x02), bypassing LVGL / esp_lcd_touch.
 * Used in the light-sleep wake path to tell a REAL finger on the screen from
 * a phantom INT during motion. */
bool hw_touch_is_touched(void)
{
    if (hw_touch_dev_get() != ESP_OK) return false;
    uint8_t pts = 0;
    if (hw_i2c_read_byte(s_tp_dev, 0x02, &pts) != ESP_OK) return false;
    return (pts & 0x0F) != 0;
}

/* Re-apply the FT3168 tuning registers (mirrors esp_lcd_touch_ft5x06's
 * touch_ft5x06_init) so the touch controller is back in a known state after
 * light sleep.  The FT3168 INT line is unreliable on this board, so during
 * sleep the ULP polls it; after ANY wake we do a full hardware re-init here
 * (hard reset -> polling mode -> tuning registers -> read-back verify). */
esp_err_t hw_touch_reinit(void)
{
    /* 0) Full hardware reset of the I2C controller + bus.  Light sleep can
     *    leave the I2C peripheral/panel_io transaction state wedged even
     *    though a FRESH device handle (our probe) reads fine — that is the
     *    "ftOK but UI touch dead" signature.  Resetting the whole controller
     *    clears any stuck transaction for EVERY handle (incl. LVGL's). */
    i2c_master_bus_handle_t bus = hw_i2c_get_bus_handle();
    if (bus) {
        i2c_master_bus_reset(bus);
        esp_rom_delay_us(2 * 1000);
    }

    /* 1) Hardware reset (active-low on GPIO9) — the controller must start
     *    from a clean state or its internal I2C engine can stay wedged.
     *    FT5x06 needs ~300 ms to boot after reset; wait generously. */
    gpio_set_direction(HW_TOUCH_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(HW_TOUCH_RST, 0);
    esp_rom_delay_us(10 * 1000);                 /* hold in reset 10 ms   */
    gpio_set_level(HW_TOUCH_RST, 1);
    esp_rom_delay_us(300 * 1000);                /* release, boot 300 ms   */

    esp_err_t ret = hw_touch_dev_get();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FT3168 device handle failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2) Put FT3168 in active polling mode (reg 0x00 = 0x00, same as the
     *    esp_lcd_touch_ft5x06 driver's touch_ft5x06_init). */
    ret = hw_i2c_write_byte(s_tp_dev, 0x00, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FT3168 set polling mode failed (%s)", esp_err_to_name(ret));
        return ret;
    }
    esp_rom_delay_us(10 * 1000);

    /* 3) Tuning registers from esp_lcd_touch_ft5x06.c touch_ft5x06_init(). */
    static const uint8_t cfg[][2] = {
        {0x80, 70}, {0x81, 60}, {0x82, 16}, {0x83, 60},
        {0x84, 10}, {0x85, 20}, {0x87, 2},  {0x88, 12}, {0x89, 40},
    };
    for (size_t i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
        ret = hw_i2c_write_byte(s_tp_dev, cfg[i][0], cfg[i][1]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "FT3168 reinit reg 0x%02X failed (%s)",
                     cfg[i][0], esp_err_to_name(ret));
            return ret;
        }
    }

    /* 4) Read-back verify: reg 0x02 = number of active touch points.  If
     *    this read fails, try a full hardware bus reset (i2c_master_bus_reset
     *    re-inits the controller) then re-write the registers and retry once —
     *    this recovers the "controller wedged mid-transaction during light
     *    sleep" case that CONFIG_PM_ENABLE cannot cover (direct
     *    esp_light_sleep_start() does not honour PM locks). */
    uint8_t tp = 0xFF;
    ret = hw_i2c_read_byte(s_tp_dev, 0x02, &tp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FT3168 read-back failed (%s) — attempting I2C bus reset",
                 esp_err_to_name(ret));
        if (bus) {
            i2c_master_bus_reset(bus);
            vTaskDelay(pdMS_TO_TICKS(10));
            /* Re-write the tuning registers after the controller reset. */
            for (size_t i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
                hw_i2c_write_byte(s_tp_dev, cfg[i][0], cfg[i][1]);
            }
            ret = hw_i2c_read_byte(s_tp_dev, 0x02, &tp);
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FT3168 still not responding after bus reset (%s)",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "FT3168 re-init OK (read-back 0x02 = 0x%02X, %u points)",
             tp, tp & 0x0F);
    return ESP_OK;
}
