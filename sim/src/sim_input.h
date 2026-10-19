/**
 * @file sim_input.h — mouse / touch / key input for the simulator.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register an LVGL pointer indev fed by the SDL mouse. */
lv_indev_t *sim_input_init(void);

/** Register an LVGL keypad indev fed by the keyboard. */
lv_indev_t *sim_input_init_keypad(void);

/** Scripted touch for headless click tests (--tap). */
void sim_input_synthetic_touch(int x, int y, bool pressed);

#ifdef __cplusplus
}
#endif
