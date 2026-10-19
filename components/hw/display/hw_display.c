/** @file hw_display.c */
#include "hw_display.h"
#include "bsp/esp-bsp.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "HW_DISPLAY";
static lv_display_t *s_disp = NULL;

esp_err_t hw_display_init(void)
{
    ESP_LOGI(TAG, "Initializing display + LVGL...");

    /* Custom LVGL port config with larger stack for WEBP/JPEG decoding
     * and for running the QuickJS engine in the LVGL task (JS_NewContext /
     * JS_Eval use a large C-stack). */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 16384;  /* internal RAM; JS gestures are shallow */
    lvgl_cfg.task_affinity = 0;   /* Pin LVGL task to core 0 so the two "swdraw"
                                   * render threads (no affinity) can run on core 1 */

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = lvgl_cfg,
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        }
    };
    s_disp = bsp_display_start_with_config(&cfg);
    if (!s_disp) { ESP_LOGE(TAG, "bsp_display_start_with_config() failed"); return ESP_FAIL; }

    /* The LVGL task is already running by now.  Hold the LVGL lock so we
     * never invalidate areas while lv_timer_handler() is mid-render on the
     * other core — LVGL asserts "Invalidate area is not allowed during
     * rendering" and spins forever in that case (task watchdog). */
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Failed to lock LVGL");
        return ESP_FAIL;
    }

    /* ── Eliminate white flash: force black into the framebuffer ───── */
    /* LVGL only renders when areas are invalidated.  A fresh display
     * has no invalid areas, so the framebuffer stays uninitialised
     * (or whatever bsp_display_start left in it) until ui_init()
     * loads a screen and calls lv_timer_handler.  The panel latches
     * whatever is in the framebuffer on its first scan-out — if that
     * is white, the user sees a white flash.
     *
     * Fix: zero-fill the active draw buffer so the very first scan-out
     * is solid black, regardless of whether LVGL has rendered yet. */
    {
        lv_draw_buf_t *buf = lv_display_get_buf_active(s_disp);
        if (buf && buf->data && buf->data_size > 0) {
            memset(buf->data, 0, buf->data_size);
        }
    }

    /* Belt-and-suspenders: also set the default screen background to
     * black in case LVGL does a partial render before ui_init(). */
    lv_obj_t *def_scr = lv_display_get_screen_active(s_disp);
    if (def_scr) {
        lv_obj_set_style_bg_color(def_scr, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(def_scr, LV_OPA_COVER, 0);
    }

    bsp_display_unlock();

    ESP_LOGI(TAG, "Display ready (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

esp_err_t hw_display_deinit(void) { return ESP_OK; }

/* Brightness is a SPI command (AMOLED 0x51), so it MUST be serialised against
 * the LVGL task's frame flushes.  The LVGL flush (esp_lcd_panel_io_spi
 * tx_color) issues a POLLING command transmit while holding the acquired SPI
 * bus; a concurrent tx_param (brightness / idle / any send_cmd) from another
 * task then hits "Cannot acquire bus when a polling transaction is in
 * progress".  If the flush then fails, lv_disp_flush_ready() is never called
 * and the LVGL task deadlocks → task watchdog.
 *
 * lvgl_port_lock is a RECURSIVE mutex, so this is also safe when called from
 * the LVGL task itself (settings slider callbacks, etc.) — it just re-acquires. */
esp_err_t hw_display_set_brightness(uint8_t pct)
{
    if (!hw_display_lock(200)) {
        ESP_LOGW(TAG, "Display lock timeout — brightness %u%% skipped", pct);
        return ESP_FAIL;
    }
    esp_err_t ret = bsp_display_brightness_set(pct);
    hw_display_unlock();
    return ret;
}

uint8_t  hw_display_get_brightness(void) { return bsp_display_brightness_get(); }
esp_err_t hw_display_backlight_on(void)  { return hw_display_set_brightness(100); }
esp_err_t hw_display_backlight_off(void) { return hw_display_set_brightness(0); }

/* MIPI DCS: Sleep In (0x10) shuts down the AMOLED driver + panel power — the
 * display current drops from several-tens of mA (still "displaying black" in
 * Display-On mode) to < 0.5 mA while the watch is in standby.  Only a REAL
 * display wake (button / touch / confirmed raise / charging) sends Sleep-Out
 * (0x11) + Display-On (0x29); housekeeping + tinyML-reject re-sleeps never
 * touch the panel, so it stays asleep through them. */
#define LCD_CMD_SLEEP_IN   0x10
#define LCD_CMD_SLEEP_OUT  0x11
#define LCD_CMD_DISPLAY_ON 0x29

esp_err_t hw_display_sleep(void) {
    ESP_LOGI(TAG, "Display → sleep (Sleep-In 0x10)");
    hw_display_set_brightness(0);
    if (!hw_display_lock(200)) {
        ESP_LOGW(TAG, "Display lock timeout — Sleep-In skipped");
        return ESP_FAIL;
    }
    esp_err_t ret = bsp_display_send_cmd(LCD_CMD_SLEEP_IN, NULL, 0);
    hw_display_unlock();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Sleep-In rejected (%s)", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(120));   /* panel needs >120 ms to enter sleep */
    return ESP_OK;
}
/* Wake: Sleep-Out + Display-On.  Brightness is ramped by the caller to the
 * user's configured level — never flash a fixed 80 here, or a low-brightness
 * user gets a bright blink on every wake. */
esp_err_t hw_display_wake(void) {
    ESP_LOGI(TAG, "Display → wake (Sleep-Out 0x11)");
    if (!hw_display_lock(200)) {
        ESP_LOGW(TAG, "Display lock timeout — Sleep-Out skipped");
        return ESP_FAIL;
    }
    esp_err_t ret = bsp_display_send_cmd(LCD_CMD_SLEEP_OUT, NULL, 0);
    if (ret == ESP_OK) {
        ret = bsp_display_send_cmd(LCD_CMD_DISPLAY_ON, NULL, 0);
    }
    hw_display_unlock();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Sleep-Out rejected (%s)", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(120));   /* panel needs >120 ms to wake */
    return ESP_OK;
}

/* Stop / resume the LVGL rendering loop so it never flushes frames to the
 * SPI panel while the display is powered down.  Without this, the LVGL task
 * keeps issuing SPI color transfers that race the main loop's brightness /
 * standby commands on the same bus → "Cannot acquire bus when a polling
 * transaction is in progress" → the LVGL task deadlocks → task-watchdog
 * freeze (seen after light sleep was removed). */
esp_err_t hw_display_pause_rendering(void)
{
    if (!hw_display_lock(500)) {
        ESP_LOGW(TAG, "Display lock timeout — cannot pause LVGL rendering");
        return ESP_FAIL;
    }
    lvgl_port_stop();   /* stops tick timer + disables LVGL timers → no flush */
    hw_display_unlock();
    ESP_LOGI(TAG, "Display: LVGL rendering paused");
    return ESP_OK;
}

esp_err_t hw_display_resume_rendering(void)
{
    if (!hw_display_lock(500)) {
        ESP_LOGW(TAG, "Display lock timeout — cannot resume LVGL rendering");
        return ESP_FAIL;
    }
    lvgl_port_resume();
    hw_display_unlock();
    ESP_LOGI(TAG, "Display: LVGL rendering resumed");
    return ESP_OK;
}

/* MIPI DCS Idle Mode ON / OFF — the AMOLED "AOD" low-refresh mode. */
#define LCD_CMD_IDLE_ON  0x39
#define LCD_CMD_IDLE_OFF 0x38

esp_err_t hw_display_set_idle(bool idle)
{
    uint8_t cmd = idle ? LCD_CMD_IDLE_ON : LCD_CMD_IDLE_OFF;
    /* Same serialisation as brightness — idle mode (0x39/0x38) is a SPI
     * command too and races the LVGL flush the same way. */
    if (!hw_display_lock(200)) {
        ESP_LOGW(TAG, "Display lock timeout — idle %s skipped", idle ? "ON" : "OFF");
        return ESP_FAIL;
    }
    esp_err_t ret = bsp_display_send_cmd(cmd, NULL, 0);
    hw_display_unlock();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Idle mode %s rejected (%s)",
                 idle ? "ON" : "OFF", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Display idle %s — ~1Hz AOD refresh",
                 idle ? "ON" : "OFF");
    }
    return ret;
}

lv_display_t *hw_display_get_lvgl_display(void) { return s_disp; }

esp_err_t hw_display_set_refr_period_ms(uint32_t ms)
{
    if (!s_disp) return ESP_FAIL;
    if (ms == 0) ms = LV_DEF_REFR_PERIOD;

    if (!hw_display_lock(200)) {
        ESP_LOGW(TAG, "Display lock timeout — refresh period unchanged");
        return ESP_FAIL;
    }
    lv_timer_t *t = lv_display_get_refr_timer(s_disp);
    if (t) {
        lv_timer_set_period(t, ms);
    }
    hw_display_unlock();

    ESP_LOGI(TAG, "Display refresh period → %" PRIu32 " ms (~%u FPS)", ms,
             (unsigned)(1000 / (ms ? ms : 1)));
    return (t) ? ESP_OK : ESP_FAIL;
}

bool hw_display_lock(int t) { return bsp_display_lock(t); }
void hw_display_unlock(void) { bsp_display_unlock(); }
