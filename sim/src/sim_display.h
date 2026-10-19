/**
 * @file sim_display.h — SDL2-backed LVGL display for the host simulator.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Panel geometry — matches the Waveshare 2.06" AMOLED. */
#define SIM_HOR_RES 410
#define SIM_VER_RES 502

/**
 * @brief Create the SDL window and register an LVGL display on it.
 * @param scale Integer window scale (1 = 410x502, 2 = 820x1004).
 * @return the LVGL display, or NULL on failure.
 */
lv_display_t *sim_display_init(int scale);

/** @return the integer window scale factor (1..4). */
int sim_window_scale(void);

/** Pump SDL events, blit the latest frame and present it. */
void sim_display_present(void);

/**
 * @brief Save the current frame to a BMP file.
 * @return true on success.
 */
bool sim_display_save_bmp(const char *path);

/** @return true while the window is open. */
bool sim_display_is_open(void);

/** Destroy the window and free buffers. */
void sim_display_deinit(void);

#ifdef __cplusplus
}
#endif
