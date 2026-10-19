/** @file hw_imu.h — QMI8658 6-axis IMU wrapper */
#pragma once
#include "hw_common.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float accel_x;    /**< m/s² */
    float accel_y;
    float accel_z;
    float gyro_x;     /**< rad/s */
    float gyro_y;
    float gyro_z;
    float temperature;
} hw_imu_data_t;

esp_err_t hw_imu_init(void);
esp_err_t hw_imu_deinit(void);
esp_err_t hw_imu_read(hw_imu_data_t *data);
esp_err_t hw_imu_sleep(void);
esp_err_t hw_imu_wake(void);

#ifdef __cplusplus
}
#endif
