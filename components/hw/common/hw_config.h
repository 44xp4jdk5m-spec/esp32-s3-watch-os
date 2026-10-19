/**
 * @file hw_config.h
 * @brief Consolidated pinout and bus addresses for ESP32-S3-Touch-AMOLED-2.06.
 */

#pragma once

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── I2C bus (shared) ──────────────────────────────────────────────────── */
#define HW_I2C_SCL              GPIO_NUM_14
#define HW_I2C_SDA              GPIO_NUM_15
#define HW_I2C_FREQ_HZ          400000

/* ── I2C device addresses ──────────────────────────────────────────────── */
#define HW_AXP2101_I2C_ADDR     0x34    /**< Power Management IC */
#define HW_FT3168_I2C_ADDR      0x38    /**< Touch controller */
#define HW_PCF85063_I2C_ADDR    0x51    /**< RTC chip */
#define HW_QMI8658_I2C_ADDR     0x6B    /**< IMU (high address) */
#define HW_ES8311_I2C_ADDR      0x18    /**< Audio DAC */
#define HW_ES7210_I2C_ADDR      0x40    /**< Audio ADC */

/* ── Display (QSPI / SPI2) ─────────────────────────────────────────────── */
#define HW_LCD_CS               GPIO_NUM_12
#define HW_LCD_PCLK             GPIO_NUM_11
#define HW_LCD_DATA0            GPIO_NUM_4
#define HW_LCD_DATA1            GPIO_NUM_5
#define HW_LCD_DATA2            GPIO_NUM_6
#define HW_LCD_DATA3            GPIO_NUM_7
#define HW_LCD_RST              GPIO_NUM_8
#define HW_LCD_H_RES            410
#define HW_LCD_V_RES            502

/* ── Touch ──────────────────────────────────────────────────────────────── */
#define HW_TOUCH_RST            GPIO_NUM_9
#define HW_TOUCH_INT            GPIO_NUM_38

/* ── I2S Audio ──────────────────────────────────────────────────────────── */
#define HW_I2S_MCLK             GPIO_NUM_16
#define HW_I2S_SCLK             GPIO_NUM_41
#define HW_I2S_LCLK             GPIO_NUM_45
#define HW_I2S_DOUT             GPIO_NUM_40
#define HW_I2S_DSIN             GPIO_NUM_42
#define HW_POWER_AMP_IO         GPIO_NUM_46

/* ── SD Card (1-bit SDMMC) ──────────────────────────────────────────────── */
#define HW_SD_D0                GPIO_NUM_3
#define HW_SD_CMD               GPIO_NUM_1
#define HW_SD_CLK               GPIO_NUM_2

/* ── Buttons ────────────────────────────────────────────────────────────── */
#define HW_BTN_BOOT_GPIO        GPIO_NUM_0      /**< BOOT button, active LOW */
#define HW_BTN_PWR_GPIO         GPIO_NUM_10     /**< PWR button via AXP2101, active HIGH, 6s=shutdown */

/* ── AXP2101 PMIC config ────────────────────────────────────────────────── */
#define HW_AXP2101_DC1_MV       3300            /**< Main system voltage 3.3V */
#define HW_AXP2101_CHG_CUR_MA   400             /**< Battery charge current */
#define HW_AXP2101_CHG_VOL_MV   4200            /**< Battery target voltage */
#define HW_AXP2101_BAT_CAP_MAH  400             /**< Battery capacity */

#ifdef __cplusplus
}
#endif
