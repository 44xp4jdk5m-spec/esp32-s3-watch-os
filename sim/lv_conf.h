/**
 * @file lv_conf.h — simulator LVGL configuration.
 *
 * Mirrors the firmware's settings where they affect rendering:
 *   16-bit colour, 15 ms refresh, float support and gesture recognition
 *   (both enabled on device — see sdkconfig.defaults).
 *
 * Anything not listed here falls back to LVGL's defaults
 * (lv_conf_internal.h).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* The Noise app builds an 8-stop background gradient (noise_ui.c).  The
 * LVGL default is 2, which overflows lv_grad_dsc_t.stops[] and corrupts
 * stops_count. */
#define LV_GRADIENT_MAX_STOPS 8

/* An LVGL assert on the host means the UI is broken.  The default handler
 * spins forever, which looks like a hang; trap instead so the failure is
 * visible (and gdb can catch it). */
#define LV_ASSERT_HANDLER __builtin_trap();

/* Single-threaded: the SDL loop calls lv_timer_handler(). */
#define LV_USE_OS LV_OS_NONE

/* Settings has a CPU/FPS overlay toggle (lv_sysmon_show_performance). */
#define LV_USE_SYSMON 1
#define LV_USE_PERF_MONITOR 1

/* Built-in allocator with a generous heap (the device uses PSRAM). */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (64U * 1024U * 1024U)

#define LV_DEF_REFR_PERIOD 15

/* Diagnostics */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* Feature parity with the firmware */
#define LV_USE_FLOAT 1
#define LV_USE_GESTURE_RECOGNITION 1
#define LV_USE_PRIVATE_API 1

#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_USE_FONT_COMPRESSED 1

/* Filesystem: 'S:' maps to the spiffs/ source folder on the host. */
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'S'
#define LV_FS_STDIO_PATH ""

#define LV_USE_LODEPNG 1
#define LV_USE_SNAPSHOT 1
#define LV_USE_QRCODE 1

/* Fonts the UI pulls in directly (the device enables these too). */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#endif /* LV_CONF_H */
