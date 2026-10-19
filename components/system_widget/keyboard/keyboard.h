/**
 * @file keyboard.h
 * @brief Reusable on-screen keyboard system widget.
 *
 * A self-contained keyboard that takes over the whole 410x502 display so
 * keys are big enough for a finger.  The layout mirrors the original
 * SquareLine keyboard design (black keys, montserrat fonts, suggestion
 * bar, textarea panel, emoji / backspace / caps icons) and every row is
 * scaled to fill the full display width with no gaps between keys.
 *
 * Any app can call keyboard_open() and get the typed text back through a
 * callback.  The text is shown in plain text (no password masking).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of the text the keyboard can hold. */
#define KEYBOARD_MAX_TEXT   96

/**
 * @brief Keyboard completion callback.
 * @param text  Entered text on Done / return, or NULL when cancelled.
 *              The pointer is only valid during the callback.
 */
typedef void (*keyboard_done_cb_t)(const char *text);

/**
 * @brief Open the keyboard as a full screen.
 *
 * Saves the currently active screen so it can be restored on close.
 * If a keyboard is already open it is closed first.
 *
 * @param initial_text  Text to pre-fill the input area (may be NULL).
 * @param cb            Completion callback (may be NULL).
 */
void keyboard_open(const char *initial_text, keyboard_done_cb_t cb);

/**
 * @brief Close the keyboard (cancel).  Restores the previous screen and
 *        invokes the done callback with NULL.
 */
void keyboard_close(void);

/**
 * @brief Check whether the keyboard is currently open.
 */
bool keyboard_is_active(void);

#ifdef __cplusplus
}
#endif
