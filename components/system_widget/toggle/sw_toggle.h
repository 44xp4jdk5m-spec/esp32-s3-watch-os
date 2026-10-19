/**
 * @file sw_toggle.h
 * @brief On/off switch row: [optional round icon] + label + LVGL switch.
 *
 * A full-width pressable row that toggles an LVGL switch.  The ON track
 * colour is configurable (default green; e.g. yellow for Low Power Mode).
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Full-featured toggle configuration. */
typedef struct {
    const char    *label;     /**< row label. */
    const void    *icon;      /**< optional leading Material glyph (NULL = none). */
    lv_color_t     icon_bg;   /**< round icon bg colour (used when icon set). */
    lv_color_t     on_color;  /**< switch ON track colour. */
    bool           checked;   /**< initial switch state. */
} sw_toggle_cfg_t;

/**
 * @brief Add a label + default-green switch row.
 * @return the switch object (callers read LV_STATE_CHECKED from it).
 */
lv_obj_t *sw_toggle_add(lv_obj_t *list, const char *label, bool checked,
                        lv_event_cb_t cb, void *ud);

/**
 * @brief Add a toggle row with optional leading icon + custom ON colour.
 * @return the switch object.
 */
lv_obj_t *sw_toggle_add_ex(lv_obj_t *list, const sw_toggle_cfg_t *cfg,
                           lv_event_cb_t cb, void *ud);

#ifdef __cplusplus
}
#endif
