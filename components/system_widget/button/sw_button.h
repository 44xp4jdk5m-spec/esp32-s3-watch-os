/**
 * @file sw_button.h
 * @brief Full-width pill action button (Reboot / Uninstall / Backup ...).
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add a full-width pill action button.
 *
 * 100% x SW_ROW_H, circle radius, dark-grey fill, centred label.  The
 * button is a flex row so callers may append extra children (e.g. a
 * spinner) next to the label.
 *
 * @param list       the list to add the button to.
 * @param txt        button label.
 * @param txt_color  label colour (e.g. SW_COLOR_RED for destructive).
 * @param cb         LV_EVENT_CLICKED handler (may be NULL).
 * @return the button object (its label is always child 0).
 */
lv_obj_t *sw_button_add(lv_obj_t *list, const char *txt,
                        lv_color_t txt_color, lv_event_cb_t cb);

#ifdef __cplusplus
}
#endif
