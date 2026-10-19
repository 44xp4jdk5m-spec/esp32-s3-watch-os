/**
 * @file sw_comment.h
 * @brief Secondary description / caption text row for lists.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add a small grey description label to @p list (full width,
 *        multi-line allowed via '\n').
 * @return the label, so callers can update its text in place.
 */
lv_obj_t *sw_comment_add(lv_obj_t *list, const char *txt);

#ifdef __cplusplus
}
#endif
