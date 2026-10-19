/**
 * @file bsp/display.h
 * @brief Host shim for the Waveshare BSP display header.
 *
 * On the device this header comes from the managed component
 * `waveshare/esp32_s3_touch_amoled_2_06`.  Only the panel geometry is
 * needed by the UI sources the simulator builds.
 */
#pragma once

#define BSP_LCD_H_RES 410
#define BSP_LCD_V_RES 502
