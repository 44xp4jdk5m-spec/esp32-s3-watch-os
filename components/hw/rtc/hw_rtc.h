/** @file hw_rtc.h — PCF85063 RTC driver (I2C 0x51) */
#pragma once
#include "hw_common.h"
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t sec, min, hour, day, wday, month, year;  /* year = 0-99 (2000+) */
} hw_rtc_time_t;

esp_err_t hw_rtc_init(void);
esp_err_t hw_rtc_deinit(void);
esp_err_t hw_rtc_set_time(const hw_rtc_time_t *time);
esp_err_t hw_rtc_get_time(hw_rtc_time_t *time);
time_t    hw_rtc_to_unix(const hw_rtc_time_t *t);
void      hw_rtc_from_unix(time_t ts, hw_rtc_time_t *t);
esp_err_t hw_rtc_sleep(void);
esp_err_t hw_rtc_wake(void);

#ifdef __cplusplus
}
#endif
