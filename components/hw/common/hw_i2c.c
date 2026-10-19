/**
 * @file hw_i2c.c
 * @brief I2C bus wrapper implementation — reuses BSP I2C bus on GPIO14/15.
 */

#include "hw_i2c.h"
#include "hw_config.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "HW_I2C";
static i2c_master_bus_handle_t s_bus_handle = NULL;
static bool s_initialized = false;

esp_err_t hw_i2c_init(void)
{
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing I2C bus (SCL=14, SDA=15)...");

    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_i2c_init() failed: 0x%x", ret);
        return ret;
    }

    s_bus_handle = bsp_i2c_get_handle();
    if (!s_bus_handle) {
        ESP_LOGE(TAG, "bsp_i2c_get_handle() returned NULL");
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "I2C bus ready");
    return ESP_OK;
}

esp_err_t hw_i2c_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    esp_err_t ret = bsp_i2c_deinit();
    s_bus_handle = NULL;
    s_initialized = false;
    return ret;
}

i2c_master_bus_handle_t hw_i2c_get_bus_handle(void)
{
    return s_bus_handle;
}

esp_err_t hw_i2c_add_device(uint8_t dev_addr, i2c_master_dev_handle_t *dev_handle)
{
    if (!s_bus_handle) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = HW_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus_handle, &dev_cfg, dev_handle);
}

esp_err_t hw_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg_addr, uint8_t *data, size_t len)
{
    if (!dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(dev, &reg_addr, 1, data, len, pdMS_TO_TICKS(100));
}

esp_err_t hw_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg_addr, const uint8_t *data, size_t len)
{
    if (!dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[len + 1];
    buf[0] = reg_addr;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(dev, buf, len + 1, pdMS_TO_TICKS(100));
}

esp_err_t hw_i2c_write_byte(i2c_master_dev_handle_t dev, uint8_t reg_addr, uint8_t value)
{
    return hw_i2c_write_reg(dev, reg_addr, &value, 1);
}

esp_err_t hw_i2c_read_byte(i2c_master_dev_handle_t dev, uint8_t reg_addr, uint8_t *value)
{
    return hw_i2c_read_reg(dev, reg_addr, value, 1);
}
