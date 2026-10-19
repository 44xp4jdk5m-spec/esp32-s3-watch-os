/**
 * @file sw_icons.h
 * @brief Material Symbols icon aliases used by the system widgets / apps.
 *
 * These are thin aliases over the MI_* codepoints from mi_symbols.h so the
 * UI code reads semantically (SW_ICON_WIFI instead of MI_WIFI) and one
 * swap in this file re-brands every icon font / glyph in one place.
 */

#pragma once

#include "mi_symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rows / navigation */
#define SW_ICON_WIFI            MI_WIFI
#define SW_ICON_BLUETOOTH       MI_BLUETOOTH
#define SW_ICON_SUN             MI_LIGHT_MODE
#define SW_ICON_NOTIFICATION    MI_NOTIFICATIONS
#define SW_ICON_VOLUME_UP       MI_VOLUME_UP
#define SW_ICON_VOLUME_DOWN     MI_VOLUME_DOWN
#define SW_ICON_VOLUME_OFF      MI_VOLUME_OFF
#define SW_ICON_LOCK            MI_LOCK
#define SW_ICON_LIST            MI_LIST
#define SW_ICON_SETTINGS        MI_SETTINGS
#define SW_ICON_ARROW_BACK      MI_ARROW_BACK
#define SW_ICON_ARROW_FWD       MI_ARROW_FORWARD
#define SW_ICON_REMOVE          MI_REMOVE
#define SW_ICON_ADD             MI_ADD
#define SW_ICON_CHECK           MI_CHECK
#define SW_ICON_WARNING         MI_WARNING
#define SW_ICON_CLOSE           MI_CLOSE
#define SW_ICON_VISIBILITY      MI_VISIBILITY
#define SW_ICON_LOCATION_ON     MI_LOCATION_ON
#define SW_ICON_HOME            MI_HOME

/* Settings sections */
#define SW_ICON_STORAGE         MI_STORAGE
#define SW_ICON_LANGUAGE        MI_LANGUAGE
#define SW_ICON_INFO            MI_INFO
#define SW_ICON_SCIENCE         MI_SCIENCE
#define SW_ICON_BUG             MI_BUG_REPORT
#define SW_ICON_POWER           MI_POWER
#define SW_ICON_DISPLAY         MI_DISPLAY_SETTINGS
#define SW_ICON_APPS            MI_APPS
#define SW_ICON_GRID_VIEW       MI_GRID_VIEW
#define SW_ICON_SCHEDULE        MI_SCHEDULE
#define SW_ICON_SD_CARD         MI_SD_CARD
#define SW_ICON_CLOUD           MI_CLOUD
#define SW_ICON_USB             MI_USB
#define SW_ICON_WIFI_FIND       MI_WIFI_FIND

/* Haptics */
#define SW_ICON_ARROW_UP        MI_ARROW_UP
#define SW_ICON_ARROW_DOWN      MI_ARROW_DOWN
#define SW_ICON_PAUSE           MI_PAUSE
#define SW_ICON_EMERGENCY       MI_EMERGENCY
#define SW_ICON_NOTIF_ACTIVE    MI_NOTIF_ACTIVE

#ifdef __cplusplus
}
#endif
