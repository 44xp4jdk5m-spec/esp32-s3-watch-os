# ── Hardware abstraction layer (HAL) ──────────────────────────────────
# Board-level drivers for the ESP32-S3 Touch AMOLED 2.06 watch:
#   I2C bus, AXP2101 PMIC, SH8601 display + FT5x06 touch, ES8311 audio,
#   QMI8658 IMU, RTC, GPIO button, vibrator.
# Pure driver layer — no app/OS dependencies.

idf_component_register(
    SRCS
        "hw_init.c"
        "common/hw_i2c.c"
        "power/hw_axp2101.c"
        "power/hw_sleep.c"
        "display/hw_display.c"
        "display/hw_touch.c"
        "audio/hw_audio.c"
        "sensors/hw_imu.c"
        "rtc/hw_rtc.c"
        "input/hw_button.c"
        "actuator/hw_vibrator.c"
    INCLUDE_DIRS
        "."
        "common"
        "power"
        "display"
        "audio"
        "sensors"
        "rtc"
        "input"
        "actuator"
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_hw_support
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2c
        esp_driver_i2s
)
