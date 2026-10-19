/**
 * @file actionsheet.h
 * @brief Action Sheet system widget (user-initiated frosted modal).
 *
 * A frosted-glass modal that blurs whatever is on screen behind it in
 * real time.  It is *active* — the user opens it from a UI action (e.g.
 * tapping a "Learn more…" link, opening a notification) — and shows:
 *   - a floating cancel X (top-left) + live clock (top-right)
 *   - a Material-symbol title
 *   - a scrollable body
 *   - a stack of frosted-grey action buttons
 *
 * The modal lives on lv_layer_top() so it floats above every app screen
 * (including the Settings header) and needs no screen save/restore on
 * close.  Content and action labels are copied internally, so the caller
 * can pass transient/stack buffers.  While open the display is kept
 * awake so the user has time to read.
 *
 * Any app can open one with action_sheet_open() and get the tapped action
 * back through a callback.  The cancel X reports idx = -1.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of action buttons an action sheet can hold. */
#define ACTION_SHEET_MAX_ACTIONS   4

/**
 * @brief Action sheet completion callback.
 *
 * @param idx        0-based index of the tapped action button, or -1 if
 *                   the sheet was cancelled (floating X button).
 * @param user_data  Opaque pointer passed to action_sheet_open().
 */
typedef void (*action_sheet_cb_t)(int idx, void *user_data);

/**
 * @brief Open the action sheet as a frosted full-screen modal.
 *
 * Blurs the current screen in real time, shows the live time top-right
 * and the floating cancel X top-left.  Title + content + action buttons
 * live in one vertical scrollable column: actions stay pinned to the
 * bottom for short content and scroll away when the content is long, so
 * they never overlap the content and never leave an extra gap.
 *
 * @param title_icon   Material-symbol codepoint for the title, e.g.
 *                     MI_INFO or MI_WARNING (may be NULL → info icon).
 * @param content      Body text (centered, wraps).  Pass localized text;
 *                     it is copied internally.
 * @param actions      Array of action labels (copied internally, may be
 *                     NULL when action_count == 0).
 * @param action_count Number of entries in @p actions
 *                     (clamped to ACTION_SHEET_MAX_ACTIONS).
 * @param cb           Completion callback (may be NULL).
 * @param user_data    Passed through to @p cb.
 */
void action_sheet_open(const char *title_icon,
                       const char *content,
                       const char *const *actions,
                       int action_count,
                       action_sheet_cb_t cb,
                       void *user_data);

/**
 * @brief Extended open: same as action_sheet_open() but renders the LAST
 *        action button in danger red (e.g. "Uninstall").
 *
 * @param destructive_last  When true, the final action button is drawn
 *                          red (0xFF3B30) instead of frosted grey.
 * @param title_icon        See action_sheet_open().
 * @param content           See action_sheet_open().
 * @param actions           See action_sheet_open().
 * @param action_count      See action_sheet_open().
 * @param cb                See action_sheet_open().
 * @param user_data         See action_sheet_open().
 */
void action_sheet_open_ex(const char *title_icon,
                          const char *content,
                          const char *const *actions,
                          int action_count,
                          bool destructive_last,
                          action_sheet_cb_t cb,
                          void *user_data);

/**
 * @brief Close the action sheet (as if cancelled).
 *
 * Invokes the completion callback with idx = -1 unless it was already
 * closed.
 */
void action_sheet_close(void);

/** @brief True while the action sheet is currently on screen. */
bool action_sheet_is_open(void);

#ifdef __cplusplus
}
#endif
