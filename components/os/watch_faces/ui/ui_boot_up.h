/**
 * @file ui_boot_up.h
 * @brief Boot-up splash screen: radial-bar progress ring.
 *
 * Shows a black screen with a centred "Watch OS" title and a ring of
 * discrete radial bars (idle gray, active white).  Bars light up white
 * clockwise from the top as progress advances.  This screen runs during the
 * risky init phase (WiFi, PMIC polling) to prevent LVGL widget conflicts
 * and avoid the default white flash.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern lv_obj_t *ui_boot_up;
extern lv_obj_t *ui_boot_label;
extern lv_obj_t *ui_boot_version;

/** Create the boot-up screen (does not load it). */
void ui_boot_up_screen_init(void);

/** Destroy the boot-up screen and all its children. */
void ui_boot_up_screen_destroy(void);

/** Start the slow progress-ramp animation. */
void ui_boot_up_start_animation(void);

/** Stop the progress animation (safe to call even if not running). */
void ui_boot_up_stop_animation(void);

/** Jump progress forward to at least pct (0-100).  Used for boot
 *  milestones (e.g. WiFi got an IP).  Progress never decreases; safe to
 *  call from any task.  No-op if the boot screen is not active. */
void ui_boot_up_bump_progress(int pct);

/** Force progress to 100% and light every panel immediately.  Call under
 *  the display lock before the boot screen is destroyed. */
void ui_boot_up_complete(void);

#ifdef __cplusplus
}
#endif
