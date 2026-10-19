/**
 * @file sw_icon_btn.h
 * @brief Row with a round colour icon + label (the main settings rows).
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add a row with a round icon button (coloured circle + glyph +
 *        text).  @p ud is passed as the LV_EVENT_CLICKED user_data.
 * @return the row object.
 */
lv_obj_t *sw_icon_btn_add(lv_obj_t *list, lv_color_t bg,
                          const void *icon, const char *txt,
                          lv_event_cb_t cb, void *ud);

#ifdef __cplusplus
}
#endif
