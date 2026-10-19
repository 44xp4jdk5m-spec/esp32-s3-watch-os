/**
 * @file sw_subpage.h
 * @brief Reusable "subpage" engine for settings-like apps.
 *
 * A subpage is the whole drill-in screen bundle: a full-screen content
 * layer, a floating navigation header (back button + title [+ clock
 * slot]), the scrollable list that holds the row widgets, the Apple-Watch
 * slide transition and the scroll-scale effect.
 *
 * The owner drives a page tree: for every page it calls sw_subpage_show()
 * with a title + a build callback that fills the list.  The owner also
 * supplies a back handler that decides "go to parent page" vs "destroy
 * (exit)" — the subpage knows nothing about the owner's page hierarchy.
 */

#pragma once

#include "lvgl.h"
#include "sw_theme.h"
#include "sw_nav_header.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sw_subpage sw_subpage_t;

/** Fill @p list with the current page's rows. */
typedef void (*sw_subpage_build_fn)(lv_obj_t *list, void *ud);

/** The header back button was pressed — owner decides what to do. */
typedef void (*sw_subpage_back_fn)(void *ud);

/**
 * @brief Create a subpage engine (captures the current screen to restore
 *        on destroy; the floating header is created immediately).
 * @param title     initial header title.
 * @param back_fn   called when the header back button is pressed.
 * @param back_ud   opaque context passed to @p back_fn.
 */
sw_subpage_t *sw_subpage_create(const char *title,
                                sw_subpage_back_fn back_fn, void *back_ud);

/**
 * @brief Build + show a new page with an Apple-Watch slide transition.
 * @param title  header title for this page.
 * @param build  builds the page rows into the list.
 * @param ud     opaque context passed to @p build (e.g. the page id).
 * @param back   true → slide out (going back), false → slide in from the
 *               right (going forward).
 */
void sw_subpage_show(sw_subpage_t *sp, const char *title,
                     sw_subpage_build_fn build, void *ud, bool back);

/** Like sw_subpage_show() but loads instantly (first page, no anim). */
void sw_subpage_show_instant(sw_subpage_t *sp, const char *title,
                             sw_subpage_build_fn build, void *ud);

/** Rebuild the current page's list in place, preserving scroll position. */
void sw_subpage_refresh(sw_subpage_t *sp);

/** The current page's list container (to add rows to). */
lv_obj_t *sw_subpage_list(const sw_subpage_t *sp);

/** The floating header (e.g. to reach its clock label slot). */
sw_header_t *sw_subpage_header(const sw_subpage_t *sp);

/** The header's clock label slot (owner runs its own timer into it). */
lv_obj_t *sw_subpage_clock_label(const sw_subpage_t *sp);

/** Cross-fade the header title to @p title. */
void sw_subpage_set_title(sw_subpage_t *sp, const char *title);

/** Show / hide the whole floating header (e.g. under a keyboard). */
void sw_subpage_set_header_visible(sw_subpage_t *sp, bool visible);

/** Teardown: stop effects, delete header, restore the previous screen. */
void sw_subpage_destroy(sw_subpage_t *sp);

#ifdef __cplusplus
}
#endif
