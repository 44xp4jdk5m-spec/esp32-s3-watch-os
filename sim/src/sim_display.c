/**
 * @file sim_display.c
 * @brief SDL2 display backend for the watch simulator.
 *
 * LVGL renders into two small RGB565 draw buffers.  Each flush is copied
 * into a full-screen CPU framebuffer, which is uploaded to an SDL texture
 * once per frame.  Keeping the framebuffer on the CPU makes partial-area
 * updates (and headless screenshots) trivial and avoids backend-specific
 * quirks with sub-rectangle texture uploads.
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_display.h"

#define BUF_LINES 40   /* scanlines per LVGL draw buffer */

static SDL_Window   *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture  *s_texture;
static lv_display_t *s_display;

static uint16_t *s_fb;          /* 410x502 RGB565, the "panel" */
static int  s_scale = 1;
static bool s_open  = true;
static bool s_dirty = true;

/* ── Flush: copy the rendered area into the CPU framebuffer ────────── */
static void sim_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);

    const int32_t x = area->x1;
    const int32_t y = area->y1;

    /* Clip to the panel (LVGL may render slightly outside for shadows). */
    const int32_t x0 = x < 0 ? 0 : x;
    const int32_t y0 = y < 0 ? 0 : y;
    const int32_t x1 = (x + w) > SIM_HOR_RES ? SIM_HOR_RES : (x + w);
    const int32_t y1 = (y + h) > SIM_VER_RES ? SIM_VER_RES : (y + h);

    for (int32_t row = y0; row < y1; row++) {
        const uint16_t *src = (const uint16_t *)px_map +
                              (size_t)(row - y) * w + (x0 - x);
        memcpy(s_fb + (size_t)row * SIM_HOR_RES + x0, src,
               (size_t)(x1 - x0) * sizeof(uint16_t));
    }

    if (lv_display_flush_is_last(disp)) s_dirty = true;
    lv_display_flush_ready(disp);
}

bool sim_display_is_open(void)
{
    return s_open;
}

int sim_window_scale(void)
{
    return s_scale;
}

lv_display_t *sim_display_init(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    s_scale = scale;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return NULL;
    }

    char title[128];
    snprintf(title, sizeof(title),
             "ESP32-S3 Watch Simulator — %dx%d (%.0f%%)",
             SIM_HOR_RES, SIM_VER_RES, scale * 100.0);

    s_window = SDL_CreateWindow(title,
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                SIM_HOR_RES * s_scale, SIM_VER_RES * s_scale,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return NULL;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
                                    SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return NULL;
    }

    s_texture = SDL_CreateTexture(s_renderer,
                                  SDL_PIXELFORMAT_RGB565,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  SIM_HOR_RES, SIM_VER_RES);
    if (!s_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return NULL;
    }
    SDL_SetTextureBlendMode(s_texture, SDL_BLENDMODE_NONE);

    s_fb = (uint16_t *)calloc((size_t)SIM_HOR_RES * SIM_VER_RES,
                              sizeof(uint16_t));
    if (!s_fb) {
        fprintf(stderr, "out of memory allocating the framebuffer\n");
        return NULL;
    }

    /* ── LVGL display ─────────────────────────────────────────────── */
    s_display = lv_display_create(SIM_HOR_RES, SIM_VER_RES);
    if (!s_display) return NULL;

    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, sim_flush);

    const size_t buf_bytes = SIM_HOR_RES * BUF_LINES * sizeof(uint16_t);
    void *buf1 = malloc(buf_bytes);
    void *buf2 = malloc(buf_bytes);
    if (!buf1 || !buf2) {
        fprintf(stderr, "out of memory allocating LVGL draw buffers\n");
        return NULL;
    }
    lv_display_set_buffers(s_display, buf1, buf2, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_open  = true;
    s_dirty = true;
    return s_display;
}

void sim_display_present(void)
{
    if (!s_window) return;

    /* Drain the event queue so the window stays responsive. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            s_open = false;
            break;
        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_ESCAPE) s_open = false;
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE) s_open = false;
            break;
        default:
            break;
        }
    }

    if (!s_dirty) return;
    s_dirty = false;

    SDL_UpdateTexture(s_texture, NULL, s_fb,
                      SIM_HOR_RES * (int)sizeof(uint16_t));
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

bool sim_display_save_bmp(const char *path)
{
    if (!s_fb) return false;

    SDL_Surface *surf = SDL_CreateRGBSurfaceFrom(
        s_fb, SIM_HOR_RES, SIM_VER_RES, 16,
        SIM_HOR_RES * (int)sizeof(uint16_t),
        0xF800, 0x07E0, 0x001F, 0);
    if (!surf) return false;

    const bool saved = SDL_SaveBMP(surf, path) == 0;
    SDL_FreeSurface(surf);
    return saved;
}

void sim_display_deinit(void)
{
    /* The dummy video driver used for headless screenshots crashes inside
     * SDL's own teardown (double free in the renderer back-end), so do not
     * call the individual destructors there. The OS reclaims everything on
     * exit anyway. */
    const char *driver = SDL_GetCurrentVideoDriver();
    const bool dummy = driver && strcmp(driver, "dummy") == 0;

    if (s_display) {
        lv_display_delete(s_display);
        s_display = NULL;
    }

    if (!dummy) {
        if (s_texture)  { SDL_DestroyTexture(s_texture);   s_texture  = NULL; }
        if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = NULL; }
        if (s_window)   { SDL_DestroyWindow(s_window);     s_window   = NULL; }
        SDL_Quit();
    } else {
        s_texture  = NULL;
        s_renderer = NULL;
        s_window   = NULL;
    }

    free(s_fb);
    s_fb = NULL;
}
