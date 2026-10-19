/** @file hw_imu.c */
#include "hw_imu.h"
#include "hw_config.h"
#include "hw_i2c.h"
#include "qmi8658.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "HW_IMU";
static qmi8658_dev_t *s_dev = NULL;

esp_err_t hw_imu_init(void)
{
    ESP_LOGI(TAG, "Initializing QMI8658 IMU...");

    s_dev = calloc(1, sizeof(qmi8658_dev_t));
    if (!s_dev) return ESP_ERR_NO_MEM;

    i2c_master_bus_handle_t bus = hw_i2c_get_bus_handle();
    if (!bus || qmi8658_init(s_dev, bus, HW_QMI8658_I2C_ADDR) != 0) {
        ESP_LOGW(TAG, "QMI8658 not found — IMU disabled");
        free(s_dev); s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    qmi8658_set_accel_range(s_dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(s_dev, QMI8658_ACCEL_ODR_125HZ);
    qmi8658_set_gyro_range(s_dev, QMI8658_GYRO_RANGE_1024DPS);
    qmi8658_set_gyro_odr(s_dev, QMI8658_GYRO_ODR_125HZ);
    qmi8658_set_accel_unit_mps2(s_dev, true);

    uint8_t id;
    qmi8658_get_who_am_i(s_dev, &id);
    ESP_LOGI(TAG, "QMI8658 ID=0x%02X — ready", id);

    qmi8658_enable_accel(s_dev, true);
    qmi8658_enable_gyro(s_dev, true);
    return ESP_OK;
}

esp_err_t hw_imu_deinit(void)
{
    if (s_dev) { free(s_dev); s_dev = NULL; }
    return ESP_OK;
}

esp_err_t hw_imu_read(hw_imu_data_t *d)
{
    if (!s_dev || !d) return ESP_ERR_INVALID_STATE;
    qmi8658_data_t raw;
    qmi8658_read_sensor_data(s_dev, &raw);
    d->accel_x = raw.accelX; d->accel_y = raw.accelY; d->accel_z = raw.accelZ;
    d->gyro_x  = raw.gyroX;  d->gyro_y  = raw.gyroY;  d->gyro_z  = raw.gyroZ;
    d->temperature = raw.temperature;
    return ESP_OK;
}

esp_err_t hw_imu_sleep(void)
{
    if (!s_dev) return ESP_OK;
    ESP_LOGI(TAG, "IMU → sleep");
    qmi8658_enable_accel(s_dev, false);
    qmi8658_enable_gyro(s_dev, false);
    return ESP_OK;
}

esp_err_t hw_imu_wake(void)
{
    if (!s_dev) return ESP_OK;
    ESP_LOGI(TAG, "IMU → wake");
    qmi8658_enable_accel(s_dev, true);
    qmi8658_enable_gyro(s_dev, true);
    return ESP_OK;
}
