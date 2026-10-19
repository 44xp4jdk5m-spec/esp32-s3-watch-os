/**
 * @file main.c
 * @brief Watch simulator entry point.
 *
 * Runs the real watch UI on the host:
 *   boot splash → watch face → (H) honeycomb launcher
 *
 * Sensor data (battery, temperature, IMU) is synthetic — see
 * sim_platform.c.  There is no ESP32, no WiFi, no BLE and no ESP-DL.
 *
 * Input mirrors the real watch — there are no face hotkeys:
 *   1 (hold)  GPIO0 / BOOT button   — click = back ↔ launcher, long = no-op
 *   2 (hold)  GPIO10 / PWR button   — click/long = control center/shutdown
 *                                     (those screens are not simulated)
 *   Esc       quit
 *   Left-drag on the face → touch; hold 450 ms → watch-face picker
 *   Right-drag mouse      → simulated wrist tilt (IMU)
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "activity_ui.h"
#include "app_config.h"
#include "app_events.h"
#include "app_lang.h"
#include "calculator_ui.h"
#include "control_center.h"
#include "flashlight_ui.h"
#include "honeycomb_menu.h"
#include "noise_ui.h"
#include "settings_ui.h"
#include "sim_display.h"
#include "sim_input.h"
#include "sim_platform.h"
#include "ui_boot_up.h"
#include "watch_face.h"
#include "watch_face_picker.h"

#define BOOT_DURATION_MS 2200
#define FRAME_MS         5

static bool     s_booting        = true;
static uint32_t s_boot_start_ms  = 0;
static uint8_t  s_face_id        = WATCH_FACE_MINIMAL;
static bool     s_open_launcher  = false;
static bool     s_open_picker    = false;
static bool     s_open_calculator = false;
static bool     s_open_activity   = false;
static bool     s_open_noise      = false;
static bool     s_open_cc         = false;
static bool     s_open_flashlight = false;
static bool     s_open_settings   = false;

/* Scripted tap (headless click tests): --tap X,Y */
static int      s_tap_x = -1;
static int      s_tap_y = -1;
static int      s_tap_phase;
static uint32_t s_tap_t0;
static uint32_t s_boot_done_ms;

/* Scripted GPIO0 click (headless close tests): --back-after MS */
static int      s_back_after = -1;
static bool     s_back_done  = false;

/* Hardware-button state: key 1 = GPIO0 (BOOT), key 2 = GPIO10 (PWR). */
static bool     s_btn_down[2];
static bool     s_btn_long[2];
static uint32_t s_btn_t0[2];

/* Headless / scripted capture. */
static const char *s_shot_path  = NULL;
static uint32_t    s_shot_after = 0;
static bool        s_shot_done  = false;

static uint32_t now_ms(void)
{
    return (uint32_t)SDL_GetTicks64();
}

/* LVGL needs a monotonic millisecond tick for timers and animations. */
static uint32_t sim_tick_cb(void)
{
    return now_ms();
}

/* ── Boot splash ──────────────────────────────────────────────────── */
static void start_boot(void)
{
    ui_boot_up_screen_init();
    lv_screen_load(ui_boot_up);
    ui_boot_up_start_animation();
    g_current_screen = APP_SCREEN_BOOT_UP;
    s_boot_start_ms = now_ms();
    s_booting = true;
}

static void open_launcher(void)
{
    honeycomb_menu_set_layout_mode(
        (honeycomb_layout_mode_t)g_config.honeycomb_layout_mode);
    honeycomb_menu_set_distinct_icons(g_config.honeycomb_distinct_icons);
    honeycomb_menu_set_pre_scaled_icons(true);
    honeycomb_menu_create();
    g_current_screen = APP_SCREEN_HONEYCOMB;
    printf("[sim] launcher opened\n");
}

static void close_launcher(void)
{
    if (!honeycomb_menu_is_active()) return;
    honeycomb_menu_destroy();
    g_current_screen = APP_SCREEN_WATCH_FACE;
    printf("[sim] launcher closed\n");
}

static void open_calculator(void)
{
    if (honeycomb_menu_is_active()) honeycomb_menu_destroy();
    calculator_ui_create();
    g_current_screen = APP_SCREEN_CALCULATOR;
    printf("[sim] calculator opened\n");
}

static void open_activity(void)
{
    if (honeycomb_menu_is_active()) honeycomb_menu_destroy();
    activity_ui_create();
    g_current_screen = APP_SCREEN_ACTIVITY;
    printf("[sim] activity opened\n");
}

static void open_noise(void)
{
    if (honeycomb_menu_is_active()) honeycomb_menu_destroy();
    noise_ui_create();
    g_current_screen = APP_SCREEN_NOISE;
    printf("[sim] noise opened\n");
}
 

/* Control center is an overlay on top of whatever is showing; GPIO10 (the
 * PWR button) opens it, exactly as app_events.c does. */
static void open_control_center(void)
{
    if (control_center_is_active()) return;
    control_center_open();
    g_current_screen = APP_SCREEN_CONTROL_CENTER;
    printf("[sim] control center opened\n");
}

static void open_flashlight(void)
{
    flashlight_ui_open();
    g_current_screen = APP_SCREEN_FLASHLIGHT;
    printf("[sim] flashlight opened\n");
}

static void open_settings(void)
{
    if (honeycomb_menu_is_active()) honeycomb_menu_destroy();
    if (!settings_ui_is_active()) settings_ui_create();
    g_current_screen = APP_SCREEN_SETTINGS;
    printf("[sim] settings opened\n");
}

static void finish_boot(void)
{
    ui_boot_up_complete();
    ui_boot_up_screen_destroy();
    watch_face_activate(s_face_id);
    g_current_screen = APP_SCREEN_WATCH_FACE;
    s_booting = false;
    s_boot_done_ms = now_ms();
    printf("[sim] boot complete — face id %u\n", s_face_id);

    if (s_open_launcher) {
        open_launcher();
    } else if (s_open_picker) {
        watch_face_picker_open();
    } else if (s_open_calculator) {
        open_calculator();
    } else if (s_open_activity) {
        open_activity();
    } else if (s_open_noise) {
        open_noise();
    } else if (s_open_cc) {
        open_control_center();
    } else if (s_open_flashlight) {
        open_control_center();
        open_flashlight();
    } else if (s_open_settings) {
        open_settings();
    }
}

/* ── Clock / battery refresh ─────────────────────────────────────── */
static void refresh_face(void)
{
    char time_str[16];
    char date_str[48];

    if (sim_use_real_time()) {
        time_t t = time(NULL);
        struct tm tm_now;
        localtime_r(&t, &tm_now);
        strftime(time_str, sizeof(time_str), "%H:%M", &tm_now);
        strftime(date_str, sizeof(date_str), "%A, %-d %b", &tm_now);
    } else {
        /* Fixed demo time so screenshots are repeatable. */
        snprintf(time_str, sizeof(time_str), "10:08");
        snprintf(date_str, sizeof(date_str), "Monday, 19 Oct");
    }

    watch_face_update_current_time(time_str, date_str);
    watch_face_update_current_battery(sim_battery_pct(),
                                      sim_battery_charging(), false);
}

/* ── Hardware buttons ─────────────────────────────────────────────── */
/* The board has two buttons: BOOT on GPIO0 (active low) and PWR on GPIO10
 * (AXP2101 PEK).  Key 1 / key 2 are held down to emulate pressing them.
 * Timing matches components/hw/input/hw_button.c: BOOT reports a long
 * press after 1 s, PWR after 2 s.  Actions mirror app_events.c as far as
 * the simulator can — the control-centre and shutdown screens are not
 * part of it, so those events are only logged. */
typedef enum { SIM_BTN_BOOT = 0, SIM_BTN_PWR = 1 } sim_btn_t;
typedef enum { SIM_BTN_CLICK, SIM_BTN_LONG_PRESS } sim_btn_event_t;

static void sim_button_event(sim_btn_t btn, sim_btn_event_t evt)
{
    if (btn == SIM_BTN_BOOT) {
        if (evt == SIM_BTN_LONG_PRESS) {
            /* app_events.c only acts on GPIO0 clicks. */
            printf("[sim] GPIO0 long-press\n");
            return;
        }
        if (g_current_screen == APP_SCREEN_CALCULATOR &&
            calculator_ui_is_active()) {
            calculator_ui_handle_back();
            g_current_screen = APP_SCREEN_WATCH_FACE;
            printf("[sim] calculator closed\n");
            return;
        }
        if (g_current_screen == APP_SCREEN_ACTIVITY && activity_ui_is_active()) {
            activity_ui_handle_back();
            g_current_screen = APP_SCREEN_WATCH_FACE;
            printf("[sim] activity closed\n");
            return;
        }
        if (g_current_screen == APP_SCREEN_NOISE && noise_ui_is_active()) {
            noise_ui_handle_back();
            g_current_screen = APP_SCREEN_WATCH_FACE;
            printf("[sim] noise closed\n");
            return;
        }
        if (settings_ui_is_active() &&
            g_current_screen == APP_SCREEN_SETTINGS) {
            settings_ui_handle_back();
            if (!settings_ui_is_active()) {
                const bool from_cc = g_settings_return_to_cc;
                g_settings_return_to_cc = false;
                g_current_screen = from_cc ? APP_SCREEN_CONTROL_CENTER
                                          : APP_SCREEN_WATCH_FACE;
                printf("[sim] settings closed\n");
            }
            return;
        }
        if (flashlight_ui_is_active()) {
            flashlight_ui_handle_back();
            g_current_screen = APP_SCREEN_CONTROL_CENTER;
            printf("[sim] flashlight closed\n");
            return;
        }
        if (control_center_is_active()) {
            control_center_handle_back();
            g_current_screen = APP_SCREEN_WATCH_FACE;
            printf("[sim] control center closed\n");
            return;
        }
        if (watch_face_picker_is_open()) {
            watch_face_picker_cancel();
        } else if (honeycomb_menu_is_active()) {
            close_launcher();
        } else {
            open_launcher();
        }
    } else {
        if (evt == SIM_BTN_LONG_PRESS) {
            printf("[sim] GPIO10 long-press — shutdown screen not simulated\n");
            return;
        }
        if (control_center_is_active()) {
            control_center_scroll_top();
            printf("[sim] control center → scroll to top\n");
        } else {
            open_control_center();
        }
    }
}

static void handle_buttons(const uint8_t *keys)
{
    static const SDL_Scancode sc[2]      = { SDL_SCANCODE_1, SDL_SCANCODE_2 };
    static const uint32_t     long_ms[2] = { 1000, 2000 };
    static const int          gpio[2]    = { 0, 10 };

    const uint32_t now = now_ms();

    for (int b = 0; b < 2; b++) {
        const bool down = keys[sc[b]] != 0;

        if (down && !s_btn_down[b]) {
            s_btn_down[b] = true;
            s_btn_long[b] = false;
            s_btn_t0[b]   = now;
            printf("[sim] GPIO%d pressed\n", gpio[b]);
        } else if (down && s_btn_down[b] && !s_btn_long[b] &&
                   (now - s_btn_t0[b]) >= long_ms[b]) {
            s_btn_long[b] = true;
            sim_button_event((sim_btn_t)b, SIM_BTN_LONG_PRESS);
        } else if (!down && s_btn_down[b]) {
            s_btn_down[b] = false;
            printf("[sim] GPIO%d released\n", gpio[b]);
            if (!s_btn_long[b]) sim_button_event((sim_btn_t)b, SIM_BTN_CLICK);
        }
    }
}

/* ── Entry point ──────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    int scale = 1;      /* 410x502 — fits small screens; use --scale 2+ if you like */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--demo-time") == 0) {
            sim_set_use_real_time(false);
        } else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            s_shot_path = argv[++i];
            if (s_shot_after == 0) s_shot_after = BOOT_DURATION_MS + 1200;
        } else if (strcmp(argv[i], "--shot-after") == 0 && i + 1 < argc) {
            s_shot_after = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--face") == 0 && i + 1 < argc) {
            s_face_id = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--launcher") == 0) {
            s_open_launcher = true;
        } else if (strcmp(argv[i], "--picker") == 0) {
            s_open_picker = true;
        } else if (strcmp(argv[i], "--calculator") == 0) {
            s_open_calculator = true;
        } else if (strcmp(argv[i], "--activity") == 0) {
            s_open_activity = true;
        } else if (strcmp(argv[i], "--noise") == 0) {
            s_open_noise = true;
        } else if (strcmp(argv[i], "--cc") == 0) {
            s_open_cc = true;
        } else if (strcmp(argv[i], "--flashlight") == 0) {
            s_open_flashlight = true;
        } else if (strcmp(argv[i], "--settings") == 0) {
            s_open_settings = true;
        } else if (strcmp(argv[i], "--tap") == 0 && i + 1 < argc) {
            sscanf(argv[++i], "%d,%d", &s_tap_x, &s_tap_y);
        } else if (strcmp(argv[i], "--back-after") == 0 && i + 1 < argc) {
            s_back_after = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: %s [options]\n", argv[0]);
            printf("  --scale N        window scale 1..4 (default 1)\n");
            printf("  --demo-time      freeze the clock at 10:08 for repeatable shots\n");
            printf("  --face N         watch face id to show after boot\n");
            printf("  --launcher       open the honeycomb launcher after boot\n");
            printf("  --picker         open the watch-face picker after boot\n");
            printf("  --calculator     open the calculator after boot\n");
            printf("  --activity       open the activity app after boot\n");
            printf("  --noise          open the noise app after boot\n");
            printf("  --settings       open the settings app after boot\n");
            printf("  --tap X,Y        tap the screen at X,Y (800 ms after boot)\n");
            printf("  --back-after MS  click GPIO0 MS after boot (tests app close paths)\n");
            printf("  --cc             open the control center after boot\n");
            printf("  --flashlight     open the control center + flashlight after boot\n");
            printf("  --shot FILE      save a BMP frame to FILE and exit\n");
            printf("  --shot-after MS  delay before --shot (default boot + 1200 ms)\n");
            return 0;
        }
    }

    lv_init();
    lv_tick_set_cb(sim_tick_cb);

    if (!sim_display_init(scale)) {
        fprintf(stderr, "failed to initialise the SDL display\n");
        return 1;
    }

    /* LV_USE_PERF_MONITOR auto-shows the FPS/CPU overlay on every display.
     * The simulator only wants it when Settings toggles it on. */
#if LV_USE_PERF_MONITOR
    lv_sysmon_hide_performance(NULL);
#endif
    sim_input_init();
    sim_input_init_keypad();

    watch_face_registry_init();
    watch_face_picker_init();
    control_center_init();
    flashlight_ui_init();
    app_lang_init();        /* reads /spiffs/usr/share/lang/<code>.json */
    printf("[sim] %u watch faces registered\n", watch_face_count());
    printf("[sim] keys: 1 = GPIO0 (BOOT), 2 = GPIO10 (PWR), Esc quit\n");
    printf("[sim] hold the left mouse button on the face for the picker\n");

    start_boot();

    uint32_t last_frame   = now_ms();
    uint32_t last_refresh = 0;

    while (sim_display_is_open()) {
        const uint32_t frame_start = now_ms();
        const uint32_t dt          = frame_start - last_frame;
        last_frame = frame_start;

        sim_display_present();          /* pumps SDL events */
        sim_platform_tick(dt);

        if (s_booting && (now_ms() - s_boot_start_ms) >= BOOT_DURATION_MS) {
            finish_boot();
            refresh_face();
        }

        if (!s_booting && (frame_start - last_refresh) >= 1000) {
            last_refresh = frame_start;
            refresh_face();
        }

        if (!s_booting) handle_buttons(SDL_GetKeyboardState(NULL));

        /* Scripted GPIO0 (BOOT) click, to exercise app close paths. */
        if (s_back_after >= 0 && !s_back_done && !s_booting &&
            (now_ms() - s_boot_done_ms) >= (uint32_t)s_back_after) {
            s_back_done = true;
            printf("[sim] GPIO0 click (scripted)\n");
            sim_button_event(SIM_BTN_BOOT, SIM_BTN_CLICK);
        }

        /* Scripted tap: press 800 ms after boot, release 120 ms later. */
        if (s_tap_x >= 0 && !s_booting) {
            const uint32_t tnow = now_ms();
            if (s_tap_phase == 0 && (tnow - s_boot_done_ms) >= 800) {
                sim_input_synthetic_touch(s_tap_x, s_tap_y, true);
                s_tap_phase = 1;
                s_tap_t0 = tnow;
                printf("[sim] tap %d,%d\n", s_tap_x, s_tap_y);
            } else if (s_tap_phase == 1 && (tnow - s_tap_t0) >= 120) {
                sim_input_synthetic_touch(s_tap_x, s_tap_y, false);
                s_tap_phase = 2;
            }
        }

        /* Launcher tile clicks set the same g_pending_* flags the firmware
         * uses; the simulator acts on them here. */
        if (g_pending_calculator) {
            g_pending_calculator = false;
            open_calculator();
        }
        if (g_pending_activity) {
            g_pending_activity = false;
            open_activity();
        }
        if (g_pending_noise) {
            g_pending_noise = false;
            open_noise();
        }
        if (g_pending_cc_flashlight) {
            g_pending_cc_flashlight = false;
            open_flashlight();
        }
        if (g_pending_cc_scroll_top) {
            g_pending_cc_scroll_top = false;
            control_center_scroll_top();
        }
        if (g_pending_settings) {
            g_pending_settings = false;
            open_settings();
        }
        if (g_pending_cc_open_settings) {
            g_pending_cc_open_settings = false;
            open_settings();
        }
        if (g_pending_cc_settings_target >= 0) {
            const settings_cc_target_t t =
                (settings_cc_target_t)g_pending_cc_settings_target;
            g_pending_cc_settings_target = -1;
            settings_ui_launch_from_cc(t);
            g_current_screen = APP_SCREEN_SETTINGS;
        }

        /* Long-press on the face opens the picker (the same 450 ms gesture
         * as the device); thumbnails then fill in one per iteration. */
        watch_face_picker_process();

        lv_timer_handler();

        if (watch_face_picker_is_open()) {
            watch_face_picker_load_next_thumb();
        }

        if (s_shot_path && !s_shot_done &&
            (now_ms() - s_boot_start_ms) >= s_shot_after) {
            s_shot_done = true;
            lv_refr_now(NULL);
            sim_display_present();
            if (sim_display_save_bmp(s_shot_path)) {
                printf("[sim] saved %s\n", s_shot_path);
            } else {
                fprintf(stderr, "[sim] failed to save %s: %s\n",
                        s_shot_path, SDL_GetError());
            }
            break;
        }

        /* Pace the loop so we don't spin a core at 100 %. */
        const uint32_t elapsed = now_ms() - frame_start;
        if (elapsed < FRAME_MS) SDL_Delay(FRAME_MS - elapsed);
    }

    printf("[sim] shutting down\n");
    sim_display_deinit();
    lv_deinit();
    return 0;
}
