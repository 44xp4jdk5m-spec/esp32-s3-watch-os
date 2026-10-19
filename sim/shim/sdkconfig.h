/**
 * @file sdkconfig.h — host stub.
 *
 * Only the handful of CONFIG_* values the UI code reads are defined.
 */
#pragma once

#define CONFIG_IDF_TARGET "esp32s3"
#define CONFIG_IDF_TARGET_ESP32S3 1

#define CONFIG_LV_COLOR_DEPTH 16
#define CONFIG_LV_COLOR_DEPTH_16 1

#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 32768
#define CONFIG_FREERTOS_HZ 1000

#define CONFIG_SPIRAM 1
#define CONFIG_SPIRAM_USE_MALLOC 1
