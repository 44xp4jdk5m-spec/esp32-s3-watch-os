/**
 * @file hw_i2c.h
 * @brief I2C wrapper for device-level register read/write.
 */

#pragma once

#include "hw_common.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hw_i2c_init(void);
esp_err_t hw_i2c_deinit(void);
i2c_master_bus_handle_t hw_i2c_get_bus_handle(void);
esp_err_t hw_i2c_add_device(uint8_t dev_addr, i2c_master_dev_handle_t *dev_handle);
esp_err_t hw_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg_addr, uint8_t *data, size_t len);
esp_err_t hw_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg_addr, const uint8_t *data, size_t len);
esp_err_t hw_i2c_write_byte(i2c_master_dev_handle_t dev, uint8_t reg_addr, uint8_t value);
esp_err_t hw_i2c_read_byte(i2c_master_dev_handle_t dev, uint8_t reg_addr, uint8_t *value);

#ifdef __cplusplus
}
#endif
