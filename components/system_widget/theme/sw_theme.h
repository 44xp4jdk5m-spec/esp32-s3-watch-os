/**
 * @file sw_theme.h
 * @brief Shared system-widget theme: colours, fonts, layout constants.
 *
 * All pixel constants are compile-time expressions derived from the
 * display metrics (screen_metrics.h), so the whole widget set stays
 * proportional to the panel and costs zero runtime cycles.
 */

#pragma once

#include "lvgl.h"
#include "screen_metrics.h"
#include "fonts.h"
#include "mi_symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Fonts ─────────────────────────────────────────────────────────── */
#define SW_FONT_S       (&noto_font_14)   /* captions / secondary text */
#define SW_FONT_M       (&noto_font_24)   /* row titles / buttons       */
#define SW_FONT_L       (&noto_font_48)   /* big values (battery %)     */
#define SW_ICON_F       (&mi_font_24)     /* Material Symbols icons     */

/* ── Colours (Apple-Watch style palette) ───────────────────────────── */
#define SW_COLOR_RED        lv_color_hex(0xFF3B30)
#define SW_COLOR_ORANGE     lv_color_hex(0xFF9500)
#define SW_COLOR_GREEN      lv_color_hex(0x04DE71)
#define SW_COLOR_TEAL       lv_color_hex(0x5AC8FA)
#define SW_COLOR_LIGHT_BLUE lv_color_hex(0x5AC8FA) /* iOS light blue (productivity) */
#define SW_COLOR_BLUE       lv_color_hex(0x2094FA)
#define SW_COLOR_PINK       lv_color_hex(0xFA114F)
#define SW_COLOR_PURPLE     lv_color_hex(0xAF52DE)  /* iOS purple  */
#define SW_COLOR_INDIGO     lv_color_hex(0x5856D6)  /* iOS indigo  */
#define SW_COLOR_CYAN       lv_color_hex(0x32ADE6)  /* iOS light blue (storage) */
#define SW_COLOR_WHITE      lv_color_hex(0xFFFFFF)
#define SW_COLOR_BLACK      lv_color_hex(0x000000)
#define SW_COLOR_DARK_GREY  lv_color_hex(0x242424)  /* row panel fill */
#define SW_COLOR_DARK_GREY_1 lv_color_hex(0x48494B)  /* row pressed bg */
#define SW_COLOR_GREY       lv_color_hex(0x9BA0AA)
#define SW_COLOR_TEXT_GREY  lv_color_hex(0xAEB4BF)
#define SW_COLOR_SYSTEM_GREY lv_color_hex(0x8E8E93) /* neutral system grey */
#define SW_COLOR_YELLOW     lv_color_hex(0xFFD60A)
#define SW_COLOR_SWITCH_TRACK  lv_color_hex(0x555555) /* off track */
#define SW_COLOR_SEP        lv_color_hex(0x3A3A3C)    /* picker separator */

/* ── Theme roles ───────────────────────────────────────────────────── */
#define SW_THEME_PRIMARY     SW_COLOR_BLUE       /* links, ticks, accents */
#define SW_THEME_SECONDARY   SW_COLOR_DARK_GREY  /* row / panel background */
#define SW_THEME_BUTTON      SW_COLOR_DARK_GREY  /* slider +/- buttons     */

/* ── Layout constants (compile-time scaled from the 410x502 design) ─── */
/* Header */
#define SW_HDR_H            EOS_WY(120)
#define SW_HDR_MASK_H       EOS_WY(125)
#define SW_HDR_MARGIN_R     EOS_WX(30)      /* clock / title right margin  */
#define SW_HDR_TITLE_W      EOS_WX(240)
#define SW_HDR_TITLE_X_OFF  (-EOS_WX(30))   /* title aligns to the margin  */
#define SW_HDR_TITLE_Y_OFF  EOS_WY(20)
#define SW_BACK_BTN_W       EOS_WX(64)
#define SW_BACK_BTN_H       EOS_WY(64)
#define SW_BACK_BTN_MARGIN_L EOS_WX(20)

/* List / rows */
#define SW_ROW_H            EOS_WY(100)     /* one list row                */
#define SW_LIST_RADIUS      EOS_WX(25)      /* row / card corner radius    */
#define SW_ITEM_RADIUS      EOS_WX(20)      /* small control radius        */
#define SW_SECTION_H        EOS_WY(30)      /* placeholder between groups  */
#define SW_LIST_H_PAD       EOS_WX(10)      /* horizontal list padding     */
#define SW_LIST_ROW_PAD     EOS_WY(8)       /* vertical gap between rows   */
/* Gap rhythm: group related widgets at the NORMAL gap, separate groups
 * with the 1.5x GROUP gap.  sw_list_add_group_gap() cancels the pad_row
 * on both sides so its height IS the exact group gap. */
#define SW_LIST_GAP         SW_LIST_ROW_PAD /* normal in-group gap         */
#define SW_LIST_GROUP_GAP   EOS_WY(12)      /* 1.5 × SW_LIST_GAP (groups)  */
#define SW_LIST_TOP_PAD     (SW_HDR_H + EOS_WY(10))
#define SW_LIST_BOTTOM_PAD  EOS_WY(100)
#define SW_ROUND_ICON       EOS_WX(50)      /* circular icon button size   */

/* Controls */
#define SW_SWITCH_W         EOS_WX(70)
#define SW_SWITCH_H         EOS_WY(36)
#define SW_SLIDER_BTN_H     SW_ROW_H

/* Graph */
#define SW_GRAPH_H          EOS_WY(200)     /* bar-graph chart height     */
#define SW_ARC_SIZE         EOS_WX(88)      /* battery arc gauge size     */

#ifdef __cplusplus
}
#endif
