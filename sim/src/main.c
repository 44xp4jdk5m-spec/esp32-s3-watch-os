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
 * Keys:
 *   1..6      switch watch face
 *   H         open / close the honeycomb launcher
 *   B         replay the boot splash
 *   R         simulate a raise-to-wake wrist motion
 *   Esc       quit
 *   Right-drag mouse → simulated wrist tilt (IMU)
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "app_config.h"
#include "honeycomb_menu.h"
#include "sim_display.h"
#include "sim_input.h"
#include "sim_platform.h"
#include "ui_boot_up.h"
#include "watch_face.h"

#define BOOT_DURATION_MS 2200
#define FRAME_MS         5

static bool     s_booting        = true;
static uint32_t s_boot_start_ms  = 0;
static uint8_t  s_face_id        = WATCH_FACE_MINIMAL;
static bool     s_open_launcher  = false;
static bool     s_prev_keys[SDL_NUM_SCANCODES];

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
    printf("[sim] launcher opened\n");
}

static void finish_boot(void)
{
    ui_boot_up_complete();
    ui_boot_up_screen_destroy();
    watch_face_activate(s_face_id);
    s_booting = false;
    printf("[sim] boot complete — face id %u\n", s_face_id);

    if (s_open_launcher) open_launcher();
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

/* ── Hotkeys ──────────────────────────────────────────────────────── */
static bool pressed_once(SDL_Scancode sc, const uint8_t *keys)
{
    const bool down = keys[sc] != 0;
    const bool edge = down && !s_prev_keys[sc];
    s_prev_keys[sc] = down;
    return edge;
}

static void handle_hotkeys(const uint8_t *keys)
{
    static const SDL_Scancode numkeys[] = {
        SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,
        SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
    };

    if (pressed_once(SDL_SCANCODE_H, keys) && !s_booting) {
        if (honeycomb_menu_is_active()) {
            honeycomb_menu_destroy();
            printf("[sim] launcher closed\n");
        } else {
            open_launcher();
        }
    }

    for (int i = 0; i < 6; i++) {
        if (!pressed_once(numkeys[i], keys)) continue;

        const uint8_t id = (uint8_t)i;
        if (id >= watch_face_count()) continue;

        s_face_id = id;
        if (!s_booting) {
            if (honeycomb_menu_is_active()) honeycomb_menu_destroy();
            watch_face_activate(s_face_id);
            refresh_face();
            printf("[sim] face → %s\n", watch_face_get(id)->name);
        }
    }

    if (pressed_once(SDL_SCANCODE_B, keys) && !s_booting) {
        start_boot();
    }
    if (pressed_once(SDL_SCANCODE_R, keys)) {
        sim_raise_wrist();
        printf("[sim] simulated raise-to-wake\n");
    }
}

/* ── Entry point ──────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    int scale = 2;

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
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: %s [options]\n", argv[0]);
            printf("  --scale N        window scale 1..4 (default 2)\n");
            printf("  --demo-time      freeze the clock at 10:08 for repeatable shots\n");
            printf("  --face N         watch face id to show after boot\n");
            printf("  --launcher       open the honeycomb launcher after boot\n");
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
    sim_input_init();
    sim_input_init_keypad();

    watch_face_registry_init();
    printf("[sim] %u watch faces registered\n", watch_face_count());
    printf("[sim] keys: 1-6 face, H launcher, B boot splash, R raise wrist, Esc quit\n");

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

        handle_hotkeys(SDL_GetKeyboardState(NULL));

        lv_timer_handler();

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
