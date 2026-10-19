/**
 * @file settings_config.c
 * @brief Data-driven page configuration (see settings_config.h).
 *
 * Colours are raw RGB hex (0xRRGGBB) because lv_color_hex() is not a
 * valid static-initializer constant expression; the renderer converts
 * them with lv_color_hex() at page-build time.
 */

#include "settings_config.h"
#include "sw_icons.h"

const settings_main_row_t settings_main_rows[] = {
    { SW_ICON_WIFI,         0x2094FA, "settings.wifi",          NULL, P_WIFI },  /* blue   */
    { SW_ICON_BLUETOOTH,    0x2094FA, "settings.bluetooth",     NULL, P_BT },   /* blue   */
    { SW_ICON_SUN,          0xFFD60A, "settings.display",       NULL, P_DISP }, /* yellow */
    { SW_ICON_POWER,        0x04DE71, "settings.power",         NULL, P_POWER },/* green  */
    { SW_ICON_NOTIFICATION, 0xFF3B30, "settings.notifications", NULL, P_NOTIF },/* red    */
    { SW_ICON_VOLUME_UP,    0xFA114F, "settings.sound_haptics", NULL, P_SOUND },/* pink   */
    { SW_ICON_APPS,         0x5AC8FA, "settings.apps",          NULL, P_APPS }, /* teal   */
    { SW_ICON_GRID_VIEW,    0xAF52DE, "settings.app_view",      NULL, P_APPVIEW },/* purple*/
    { SW_ICON_STORAGE,      0x32ADE6, "settings.storage",       NULL, P_STORAGE },/* cyan   */
    { SW_ICON_SCHEDULE,     0xFF9500, "settings.time_region",   NULL, P_TIME_REGION }, /* orange */
    { SW_ICON_VISIBILITY,   0x5856D6, NULL, "Accessibility & Gesture", P_ACCESSIBILITY }, /* indigo */
    { SW_ICON_INFO,         0x8E8E93, NULL, "System",             P_SYSTEM },  /* grey   */
    { SW_ICON_SETTINGS,     0x9BA0AA, "settings.general",       NULL, P_GENERAL }, /* grey */
};

const int settings_main_rows_count =
    (int)(sizeof(settings_main_rows) / sizeof(settings_main_rows[0]));
