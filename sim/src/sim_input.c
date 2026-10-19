/**
 * @file sim_input.c
 * @brief Mouse → touch and keyboard → buttons input for the simulator.
 *
 * The LCD panel is 410x502; the SDL window may be scaled, so coordinates
 * are divided by the scale factor before being handed to LVGL.
 */
#include <SDL2/SDL.h>

#include "hw_touch.h"
#include "sim_display.h"
#include "sim_input.h"
#include "sim_platform.h"


/* The pointer indev also backs hw_touch_get_indev(), which the watch-face
 * picker polls for its long-press gesture. */
static lv_indev_t *s_touch_indev = NULL;

/* ── Pointer (mouse = finger) ─────────────────────────────────────── */
static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    static lv_point_t last = { 0, 0 };
    int x = 0, y = 0;
    uint32_t buttons = SDL_GetMouseState(&x, &y);

    const int scale = sim_window_scale();
    x /= scale;
    y /= scale;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= SIM_HOR_RES) x = SIM_HOR_RES - 1;
    if (y >= SIM_VER_RES) y = SIM_VER_RES - 1;

    last.x = (lv_coord_t)x;
    last.y = (lv_coord_t)y;

    data->point  = last;
    data->state  = (buttons & SDL_BUTTON_LMASK) ? LV_INDEV_STATE_PRESSED
                                                : LV_INDEV_STATE_RELEASED;

    /* Dragging with the right button simulates wrist tilt (IMU). */
    if (buttons & SDL_BUTTON_RMASK) {
        sim_set_tilt((x - SIM_HOR_RES / 2) * 0.1f,
                     (y - SIM_VER_RES / 2) * 0.1f);
    }
}

/* ── Keypad (hardware buttons) ────────────────────────────────────── */
static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    static uint32_t last_key = 0;
    const uint8_t *keys = SDL_GetKeyboardState(NULL);

    data->state = LV_INDEV_STATE_RELEASED;
    data->key   = last_key;

    struct { SDL_Scancode sc; uint32_t key; } map[] = {
        { SDL_SCANCODE_UP,     LV_KEY_UP     },
        { SDL_SCANCODE_DOWN,   LV_KEY_DOWN   },
        { SDL_SCANCODE_LEFT,   LV_KEY_LEFT   },
        { SDL_SCANCODE_RIGHT,  LV_KEY_RIGHT  },
        { SDL_SCANCODE_RETURN, LV_KEY_ENTER  },
        { SDL_SCANCODE_SPACE,  LV_KEY_ENTER  },
        { SDL_SCANCODE_ESCAPE, LV_KEY_ESC    },
        { SDL_SCANCODE_TAB,    LV_KEY_NEXT   },
        { SDL_SCANCODE_BACKSPACE, LV_KEY_BACKSPACE },
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (keys[map[i].sc]) {
            data->key   = map[i].key;
            last_key    = map[i].key;
            data->state = LV_INDEV_STATE_PRESSED;
            break;
        }
    }
}

lv_indev_t *sim_input_init(void)
{
    lv_indev_t *indev = lv_indev_create();
    if (!indev) return NULL;
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, pointer_read);
    lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);
    s_touch_indev = indev;
    return indev;
}

/* Firmware API: the picker polls this indev to detect a 450 ms long-press.
 * On the host the mouse plays the part of the touch panel. */
lv_indev_t *hw_touch_get_indev(void)
{
    return s_touch_indev;
}

lv_indev_t *sim_input_init_keypad(void)
{
    lv_indev_t *indev = lv_indev_create();
    if (!indev) return NULL;
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, keypad_read);
    lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);
    return indev;
}
