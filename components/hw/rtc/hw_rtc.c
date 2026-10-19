/**
 * @file hw_rtc.c — PCF85063 RTC driver.
 * Registers: 0x04=sec, 0x05=min, 0x06=hour, 0x07=day, 0x08=wday, 0x09=month, 0x0A=year
 * All time values in BCD. Control: 0x00 (bit7=STOP), 0x01.
 */

#include "hw_rtc.h"
#include "hw_config.h"
#include "hw_i2c.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HW_RTC";
static i2c_master_dev_handle_t s_dev = NULL;

static uint8_t b2d(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t d2b(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

esp_err_t hw_rtc_init(void)
{
    ESP_LOGI(TAG, "Initializing PCF85063 RTC...");

    HW_CHECK(hw_i2c_add_device(HW_PCF85063_I2C_ADDR, &s_dev));

    /* Clear STOP bit, disable alarm/CLKOUT */
    hw_i2c_write_byte(s_dev, 0x00, 0x00);
    hw_i2c_write_byte(s_dev, 0x01, 0x00);

    /* Check oscillator */
    uint8_t sec;
    hw_i2c_read_byte(s_dev, 0x04, &sec);
    if (sec & 0x80) ESP_LOGW(TAG, "Oscillator was stopped — time may be invalid");

    hw_rtc_time_t t;
    if (hw_rtc_get_time(&t) == ESP_OK) {
        ESP_LOGI(TAG, "RTC: %02d:%02d:%02d %04d-%02d-%02d",
                 t.hour, t.min, t.sec, 2000+t.year, t.month, t.day);
    }
    return ESP_OK;
}

esp_err_t hw_rtc_deinit(void)
{
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    return ESP_OK;
}

esp_err_t hw_rtc_set_time(const hw_rtc_time_t *t)
{
    if (!s_dev || !t) return ESP_ERR_INVALID_STATE;

    hw_i2c_write_byte(s_dev, 0x00, 0x80);  /* STOP oscillator */

    uint8_t buf[7] = {
        d2b(t->sec), d2b(t->min), d2b(t->hour),
        d2b(t->day), t->wday, d2b(t->month), d2b(t->year)
    };
    esp_err_t ret = hw_i2c_write_reg(s_dev, 0x04, buf, 7);

    hw_i2c_write_byte(s_dev, 0x00, 0x00);  /* START oscillator */
    return ret;
}

esp_err_t hw_rtc_get_time(hw_rtc_time_t *t)
{
    if (!s_dev || !t) return ESP_ERR_INVALID_STATE;

    uint8_t buf[7];
    HW_CHECK(hw_i2c_read_reg(s_dev, 0x04, buf, 7));

    t->sec   = b2d(buf[0] & 0x7F);
    t->min   = b2d(buf[1] & 0x7F);
    t->hour  = b2d(buf[2] & 0x3F);
    t->day   = b2d(buf[3] & 0x3F);
    t->wday  = buf[4] & 0x07;
    t->month = b2d(buf[5] & 0x1F);
    t->year  = b2d(buf[6]);
    return ESP_OK;
}

time_t hw_rtc_to_unix(const hw_rtc_time_t *t)
{
    struct tm tm = { .tm_sec = t->sec, .tm_min = t->min, .tm_hour = t->hour,
                     .tm_mday = t->day, .tm_mon = t->month - 1,
                     .tm_year = t->year + 100 };
    /* Use gmtime-based mktime to avoid timezone dependency.
     * mktime() assumes 'local' time and applies TZ offset —
     * but our RTC stores UTC. Temporarily set TZ=UTC during conversion. */
    char *old_tz = getenv("TZ");
    setenv("TZ", "", 1);
    tzset();
    time_t result = mktime(&tm);
    if (old_tz) {
        setenv("TZ", old_tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    return result;
}

void hw_rtc_from_unix(time_t ts, hw_rtc_time_t *t)
{
    /* Use gmtime_r to avoid timezone dependency.
     * localtime() applies the current TZ offset —
     * but the RTC stores UTC so we always want GMT. */
    struct tm tm_buf;
    struct tm *tm = gmtime_r(&ts, &tm_buf);
    if (!tm) {
        memset(t, 0, sizeof(*t));
        return;
    }
    t->sec = tm->tm_sec; t->min = tm->tm_min; t->hour = tm->tm_hour;
    t->day = tm->tm_mday; t->wday = tm->tm_wday;
    t->month = tm->tm_mon + 1; t->year = tm->tm_year - 100;
}

esp_err_t hw_rtc_sleep(void) { return ESP_OK; } /* PCF85063 always ultra-low-power */
esp_err_t hw_rtc_wake(void) { return ESP_OK; }
