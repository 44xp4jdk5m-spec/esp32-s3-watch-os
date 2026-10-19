/**
 * @file sw_title.h
 * @brief Grey section heading row (a plain label inside a list).
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add a section-title label to @p list (grey, left, full width).
 * @return the label (callers may update its text later).
 */
lv_obj_t *sw_title_add(lv_obj_t *list, const char *txt);

#ifdef __cplusplus
}
#endif
