/**
 * @file warning.h
 * @brief Warning system widget (passive frosted modal).
 *
 * A passive frosted-glass modal: it appears on its own (system events,
 * error conditions, thermal/health alerts) rather than from a user
 * action.  Unlike the action sheet it has NO floating cancel button —
 * only the floating live clock top-right — and shows the warning symbol
 * plus a green "OK" primary button and a grey "Cancel" secondary button,
 * matching the SquareLine warning design.
 *
 * Lives on lv_layer_top() like the action sheet, so it floats above every
 * app screen.  Content is copied internally and the display is kept awake
 * while it is open.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Warning completion callback.
 *
 * @param idx        0 = OK (primary, green), 1 = Cancel (secondary, grey).
 * @param user_data  Opaque pointer passed to warning_show().
 */
typedef void (*warning_cb_t)(int idx, void *user_data);

/**
 * @brief Show the passive warning modal.
 *
 * @param content    Body text (centered, wraps); copied internally.
 * @param cb         Completion callback (may be NULL).
 * @param user_data  Passed through to @p cb.
 */
void warning_show(const char *content, warning_cb_t cb, void *user_data);

/**
 * @brief Dismiss the warning (as if Cancel).
 *
 * Invokes the completion callback with idx = 1 unless it was already
 * closed.
 */
void warning_hide(void);

/** @brief True while the warning is currently on screen. */
bool warning_is_active(void);

#ifdef __cplusplus
}
#endif
