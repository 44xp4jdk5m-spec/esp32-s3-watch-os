/**
 * @file settings_config.h
 * @brief Data-driven page configuration (config layer, no rendering).
 *
 * The main Settings tree and other simple pages are declared here as plain
 * tables (icon, colour, label, target).  The UI layer just walks the table
 * and renders rows — reordering / adding a page is a one-line table edit.
 */

#pragma once

#include "lvgl.h"
#include "settings_ui_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One row of the main Settings page (round icon button). */
typedef struct {
    const char *icon;      /* Material Symbol glyph (SW_ICON_*)       */
    uint32_t    color;     /* icon circle colour as RGB hex (0xRRGGBB) */
    const char *label_key; /* app_lang() key; NULL = use literal      */
    const char *literal;   /* used when label_key is NULL             */
    page_t      target;    /* page to navigate to on click            */
} settings_main_row_t;

/** The main Settings page tree. */
extern const settings_main_row_t settings_main_rows[];
extern const int settings_main_rows_count;

#ifdef __cplusplus
}
#endif
