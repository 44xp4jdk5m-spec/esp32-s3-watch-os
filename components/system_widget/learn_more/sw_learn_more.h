/**
 * @file sw_learn_more.h
 * @brief Blue "Learn more..." link label for lists.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add a blue "Learn more..." clickable label to @p list.
 * @param cb  LV_EVENT_CLICKED handler.
 */
lv_obj_t *sw_learn_more_add(lv_obj_t *list, lv_event_cb_t cb);

#ifdef __cplusplus
}
#endif
