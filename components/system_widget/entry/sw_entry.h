/**
 * @file sw_entry.h
 * @brief Navigation row: label + right chevron.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add a row that navigates to another page (text + right chevron).
 * @return the row object.
 */
lv_obj_t *sw_entry_add(lv_obj_t *list, const char *txt,
                       lv_event_cb_t cb, void *ud);

#ifdef __cplusplus
}
#endif
