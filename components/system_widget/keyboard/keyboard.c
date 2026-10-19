/**
 * @file keyboard.c
 * @brief Reusable on-screen keyboard system widget.
 *
 * Faithfully reproduces the original SquareLine keyboard design (reference
 * copy in /tem/ui_keyboard.c): black keys with montserrat fonts, the
 * suggestion bar, the textarea input panel, and the caps / emoji / backspace
 * icons.  Generated cleanly from arrays + loops.
 *
 * Layout & scaling:
 *   - The q–p row is scaled to fill the full 410 px width (no side gaps).
 *   - Every other row (a–l, z–m, and the emoji / space / enter bottom row)
 *     uses the SAME scale ratio, so all keys are consistently sized.  Each
 *     row is then centred, so the a–l row keeps clear gaps at both ends and
 *     the bottom row sits comfortably inside the round display.
 *   - The whole keyboard is shifted down so the bottom row ends only a few
 *     px above the screen edge (round display).
 *
 * Other features:
 *   - "123" mode: the q–p row becomes 0–9 and the remaining rows become
 *     punctuation; the "123" key becomes "ABC" to switch back.
 *   - Input is a real LVGL textarea with a thin white blinking cursor,
 *     shown in plain text (no password mode).
 *   - Backspace button (icon) in the input panel; emoji, space and an enter
 *     key (LV_SYMBOL_NEW_LINE) in the bottom row.
 *   - Live auto-correct: the three suggestion slots show prefix completions
 *     from a compact dictionary (app_dictionary, SPIFFS → PSRAM), with
 *     weighted-Damerau-Levenshtein typo correction as a fallback.  Tapping
 *     a slot replaces the current word.
 *   - The pressed key itself turns grey, and an Apple-style key popup
 *     (bigger grey key + stem, follows the finger) appears above it.
 *   - Short haptic feedback on every key press (respects Silent Mode).
 *
 * The keyboard takes over the whole screen (keyboard_open) and restores
 * the previous screen when closed.
 */

#include "keyboard.h"
#include "lvgl.h"
#include "app_events.h"
#include "app_dictionary.h"
#include "esp_log.h"
#include "kbd_icons.h"
#include <string.h>

static const char *TAG = "KEYBOARD";

/* ====================================================================
 * Fonts — LVGL montserrat built-ins, enabled in sdkconfig
 * ==================================================================== */
#define KBD_FONT_S   (&lv_font_montserrat_12)   /* "123" key label */
#define KBD_FONT_M   (&lv_font_montserrat_22)   /* key labels */
#define KBD_FONT_L   (&lv_font_montserrat_26)   /* Cancel / Done */
#define KBD_FONT_TX  (&lv_font_montserrat_28)   /* textarea text */
#define KBD_FONT_XXL (&lv_font_montserrat_48)   /* key popup character */

/* ── Colours (match the existing design) ───────────────────────────── */
#define KBD_COLOR_BG           lv_color_hex(0x000000)
#define KBD_COLOR_TEXT         lv_color_hex(0xFFFFFF)
#define KBD_COLOR_BLUE         lv_color_hex(0x0689FF)
#define KBD_COLOR_GREY_SEP     lv_color_hex(0xA5A5A5)
#define KBD_COLOR_KEY_PRESSED  lv_color_hex(0x4A4A4C)   /* pressed key turns grey */
#define KBD_COLOR_POPUP        lv_color_hex(0x4A4A4C)

/* ── Layout constants (410x502, round-display aware) ───────────────── */
#define KBD_W            410
#define KBD_H            502
#define KBD_KEY_H        55
#define KBD_PANEL2_TOP   224    /* suggestion bar top (shifted down) */
#define KBD_PANEL2_H     311
#define KBD_DOWN_SHIFT   33     /* pushes the whole keyboard down so the
                                 * bottom row ends a few px above the
                                 * screen bottom (round display). */
#define KBD_QP_REF_SUM   347    /* sum of the q–p reference widths — this
                                 * row is scaled to fill 410 exactly. */
#define KBD_SCALE        ((double)KBD_W / (double)KBD_QP_REF_SUM)

/* ====================================================================
 * Key model — reference widths copied from /tem/ui_keyboard.c.  The q–p
 * row defines the scale (KBD_SCALE); every other row uses the same ratio
 * and is centred.
 * ==================================================================== */
typedef enum {
    KBD_KEY_CHAR,     /* inserts label text */
    KBD_KEY_CAPS,     /* toggle caps lock */
    KBD_KEY_MODE,     /* toggle 123 / ABC */
    KBD_KEY_SPACE,    /* insert space */
    KBD_KEY_RETURN,   /* submit (same as Done) */
    KBD_KEY_EMOJI,    /* emoji placeholder */
} kbd_key_type_t;

typedef struct {
    const char     *label;   /* text label (NULL for image-only keys) */
    kbd_key_type_t  type;
    uint16_t        w;       /* reference width (scaled by KBD_SCALE) */
    uint16_t        h;
    bool            icon;    /* render the caps / emoji image asset */
} kbd_key_def_t;

typedef struct {
    const kbd_key_def_t *keys;
    uint8_t              count;
    int16_t              y;     /* absolute top position of the row */
    bool                 fill;  /* true = scale to fill 410 exactly (q–p) */
} kbd_row_t;

#define KCHAR(l,w)   { l, KBD_KEY_CHAR, w, KBD_KEY_H, false }
#define KMODE(l,w)   { l, KBD_KEY_MODE, w, KBD_KEY_H, false }
#define KICON(t,w)   { NULL, t, w, KBD_KEY_H, true }

/* ── Letters layout ────────────────────────────────────────────────── */
static const kbd_key_def_t kbd_row_q[] = {
    KCHAR("q",40), KCHAR("w",40), KCHAR("e",33), KCHAR("r",33), KCHAR("t",33),
    KCHAR("y",33), KCHAR("u",33), KCHAR("i",33), KCHAR("o",33), KCHAR("p",36),
};
static const kbd_key_def_t kbd_row_a[] = {
    KCHAR("a",35), KCHAR("s",35), KCHAR("d",35), KCHAR("f",35), KCHAR("g",31),
    KCHAR("h",38), KCHAR("j",29), KCHAR("k",35), KCHAR("l",27),
};
static const kbd_key_def_t kbd_row_z[] = {
    KICON(KBD_KEY_CAPS,40), KCHAR("z",33), KCHAR("x",36), KCHAR("c",33), KCHAR("v",36),
    KCHAR("b",35), KCHAR("n",36), KCHAR("m",43), KMODE("123",40),
};
static const kbd_key_def_t kbd_row_bottom[] = {
    {NULL, KBD_KEY_EMOJI, 40, 59, true},
    {"SPACE", KBD_KEY_SPACE, 96, 59, false},
    {LV_SYMBOL_NEW_LINE, KBD_KEY_RETURN, 40, 59, false},
};

/* ── Numbers / punctuation layout (123 mode) ───────────────────────── */
static const kbd_key_def_t kbd_row_num[] = {
    KCHAR("1",40), KCHAR("2",40), KCHAR("3",33), KCHAR("4",33), KCHAR("5",33),
    KCHAR("6",33), KCHAR("7",33), KCHAR("8",33), KCHAR("9",33), KCHAR("0",36),
};
static const kbd_key_def_t kbd_row_sym1[] = {
    KCHAR("-",35), KCHAR("/",35), KCHAR(":",35), KCHAR(";",35), KCHAR("(",31),
    KCHAR(")",38), KCHAR("$",29), KCHAR("&",35), KCHAR("@",27),
};
static const kbd_key_def_t kbd_row_sym2[] = {
    KCHAR(".",40), KCHAR("?",33), KCHAR("!",36), KCHAR("'",33), KCHAR("\"",36),
    KCHAR(",",35), KCHAR(";",36), KCHAR(":",43), KMODE("ABC",40),
};

/* Row Y positions: the whole keyboard is shifted down (KBD_DOWN_SHIFT)
 * so the bottom row sits a few px above the screen bottom.  The q–p row
 * (and the 1..0 row in 123 mode) fills the width; the rest use the same
 * ratio and are centred. */
static const kbd_row_t kbd_rows_alpha[] = {
    { kbd_row_q,      10, 273, true  },
    { kbd_row_a,       9, 328, false },
    { kbd_row_z,       9, 383, false },
    { kbd_row_bottom,  3, 438, false },
};
static const kbd_row_t kbd_rows_sym[] = {
    { kbd_row_num,    10, 273, true  },
    { kbd_row_sym1,    9, 328, false },
    { kbd_row_sym2,    9, 383, false },
    { kbd_row_bottom,  3, 438, false },
};

/* ====================================================================
 * State
 * ==================================================================== */
static lv_obj_t            *s_scr = NULL;         /* keyboard screen */
static lv_obj_t            *s_prev_screen = NULL; /* screen to restore */
static lv_obj_t            *s_textarea = NULL;    /* input textarea */
static lv_obj_t            *s_keys_layer = NULL;  /* holds the key buttons */
static lv_obj_t            *s_sugg[3] = {NULL, NULL, NULL}; /* suggestion slots */
static lv_obj_t            *s_sep[2] = {NULL, NULL};        /* suggestion "|" separators */
static const char          *s_sugg_words[3] = {NULL, NULL, NULL}; /* dict words */
static int                  s_sugg_count = 0;
static lv_obj_t            *s_word_hl = NULL;  /* blue box behind the word being corrected */
static lv_obj_t            *s_input_panel = NULL;  /* input textarea's parent panel */
static bool                 s_ta_event = false;    /* inside the textarea's own event? */
static keyboard_done_cb_t   s_cb = NULL;
static bool                 s_active = false;
static bool                 s_caps = false;
static bool                 s_sym_mode = false;

/* Apple-style key popup (bigger grey key + stem, on the top layer) */
typedef struct {
    lv_obj_t *pop;      /* container */
    lv_obj_t *head;     /* big grey key */
    lv_obj_t *stem;     /* connector to the pressed key */
    lv_obj_t *label;    /* enlarged character */
    lv_obj_t *host;     /* key that opened the popup */
} kbd_popup_t;
static kbd_popup_t s_pop = {0};

/* ── Forward declarations ──────────────────────────────────────────── */
static void kbd_build_keys(void);
static void kbd_finish(void);
static void kbd_cancel_cb(lv_event_t *e);
static void kbd_done_cb(lv_event_t *e);
static void kbd_backspace_cb(lv_event_t *e);
static void kbd_update_suggestions(void);
static void kbd_clear_suggestions(void);
static void kbd_auto_accept(void);
static void kbd_sugg_cb(lv_event_t *e);

/* ====================================================================
 * Input helpers (drive the textarea — plain text, no password mode)
 * ==================================================================== */
static void kbd_insert_char(char c)
{
    if (!s_textarea) return;
    char ch = c;
    if (s_caps && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch == ' ') {
        /* Space commits the current word — auto-accept the top suggestion. */
        kbd_auto_accept();
    }
    lv_textarea_add_char(s_textarea, ch);
    /* Live auto-correct: a letter updates the suggestions; a separator
     * (space / punctuation) ends the current word. */
    if (ch >= 'a' && ch <= 'z') {
        kbd_update_suggestions();
    } else {
        kbd_clear_suggestions();
    }
}

static void kbd_backspace(void)
{
    if (!s_textarea) return;
    lv_textarea_delete_char(s_textarea);
    kbd_update_suggestions();
}

/* ====================================================================
 * Auto-correct / recommendation (todo points 4-5)
 * ==================================================================== */

static bool kbd_is_sep(char c)
{
    return c == ' ' || c == '.' || c == ',' || c == '!' || c == '?' || c == ';' ||
           c == ':' || c == '-' || c == '/' || c == '(' || c == ')' || c == '$' ||
           c == '&' || c == '@' || c == '"' || c == '\'';
}

/* Copy the current (incomplete) word — the text since the last separator. */
static void kbd_current_word(char *out, size_t size)
{
    out[0] = '\0';
    if (!s_textarea) return;
    const char *txt = lv_textarea_get_text(s_textarea);
    if (!txt) return;
    size_t len = strlen(txt);
    size_t start = len;
    while (start > 0 && !kbd_is_sep(txt[start - 1])) start--;
    size_t wlen = len - start;
    if (wlen >= size) wlen = size - 1;
    memcpy(out, txt + start, wlen);
    out[wlen] = '\0';
}

/* Blue highlight box behind the current word while it differs from the
 * first suggestion (i.e. it will be auto-corrected on space / enter). */
static void kbd_update_highlight(void)
{
    if (!s_word_hl || !s_textarea) return;
    char word[APP_DICT_MAX_WORD + 1];
    kbd_current_word(word, sizeof(word));

    bool show = (word[0] != '\0' && s_sugg_count > 0 && s_sugg_words[0] &&
                 strcmp(word, s_sugg_words[0]) != 0);
    if (!show) {
        lv_obj_add_flag(s_word_hl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* x = textarea origin (16) + its left pad (4) + the pixel width of
     * everything before the current word, pulled back a little so the box
     * overhangs the word on both sides. */
    const char *txt = lv_textarea_get_text(s_textarea);
    size_t len = strlen(txt);
    size_t start = len;
    while (start > 0 && !kbd_is_sep(txt[start - 1])) start--;

    char prefix[KEYBOARD_MAX_TEXT + 1];
    memcpy(prefix, txt, start);
    prefix[start] = '\0';

    lv_point_t _kbd_txt_sz;
    lv_text_get_size(&_kbd_txt_sz, prefix, KBD_FONT_TX, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_coord_t pre_w = _kbd_txt_sz.x;
    lv_text_get_size(&_kbd_txt_sz, word, KBD_FONT_TX, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_coord_t word_w = _kbd_txt_sz.x;
    lv_coord_t line_h = lv_font_get_line_height(KBD_FONT_TX);

    /* The textarea's label scrolls left as the input grows, so read its
     * live (absolute) coords to track the real on-screen token position
     * even while the text is scrolled. */
    if (!s_ta_event) lv_obj_update_layout(s_textarea);
    lv_obj_t *label = lv_textarea_get_label(s_textarea);
    lv_area_t la;
    lv_obj_get_coords(label, &la);
    lv_coord_t word_abs_x = la.x1 + pre_w - 4;   /* abs x of the word start */

    /* lv_obj_set_pos is relative to the panel's content area (pad offset). */
    lv_coord_t x = 0;
    if (s_input_panel) {
        lv_area_t pa;
        lv_obj_get_coords(s_input_panel, &pa);
        x = word_abs_x - (pa.x1 + lv_obj_get_style_pad_left(s_input_panel, 0));
    }

    lv_coord_t w = word_w + 8;
    if (w < 24) w = 24;
    lv_coord_t h = line_h + 6;
    lv_coord_t y = (50 - line_h) / 2 - 3;

    lv_obj_set_pos(s_word_hl, x, y);
    lv_obj_set_size(s_word_hl, w, h);
    if (lv_obj_get_index(s_word_hl) != 0) lv_obj_move_background(s_word_hl);
    lv_obj_remove_flag(s_word_hl, LV_OBJ_FLAG_HIDDEN);
}

/* Position the suggestion slots + separators for the current count and
 * refresh their text: 1 → single centred slot, 2 → two slots either side
 * of a centred "|", 3 → the full original layout. */
static void kbd_apply_suggestion_ui(void)
{
    int n = s_sugg_count;
    for (int i = 0; i < 3; i++) {
        if (!s_sugg[i]) continue;
        if (i >= n || !s_sugg_words[i]) {
            lv_label_set_text(s_sugg[i], "");
            lv_obj_add_flag(s_sugg[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_label_set_text(s_sugg[i], s_sugg_words[i]);
        lv_obj_remove_flag(s_sugg[i], LV_OBJ_FLAG_HIDDEN);
        if (n == 1) {
            lv_obj_center(s_sugg[i]);
        } else if (n == 2) {
            lv_obj_align(s_sugg[i], LV_ALIGN_CENTER, (i == 0) ? -70 : 70, 0);
        } else {
            lv_obj_align(s_sugg[i], LV_ALIGN_CENTER,
                         (i == 0) ? -136 : (i == 1) ? 1 : 136, 0);
        }
    }

    for (int i = 0; i < 2; i++) {
        if (!s_sep[i]) continue;
        if (n == 3) {
            lv_obj_remove_flag(s_sep[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(s_sep[i], LV_ALIGN_LEFT_MID, (i == 0) ? 136 : 273, 0);
        } else if (n == 2 && i == 0) {
            lv_obj_remove_flag(s_sep[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(s_sep[i], LV_ALIGN_CENTER, 0, 0);
        } else {
            lv_obj_add_flag(s_sep[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    kbd_update_highlight();
}

static void kbd_clear_suggestions(void)
{
    s_sugg_count = 0;
    for (int i = 0; i < 3; i++) s_sugg_words[i] = NULL;
    kbd_apply_suggestion_ui();
}

/* Refresh the suggestion slots from the dictionary (prefix match first,
 * weighted typo correction as a fallback). */
static void kbd_update_suggestions(void)
{
    char word[APP_DICT_MAX_WORD + 1];
    kbd_current_word(word, sizeof(word));
    s_sugg_count = 0;
    for (int i = 0; i < 3; i++) s_sugg_words[i] = NULL;

    if (word[0] != '\0' && app_dict_ready()) {
        app_dict_entry_t res[APP_DICT_MAX_RES];
        int n = app_dict_suggest(word, res, APP_DICT_MAX_RES);
        if (n == 0) n = app_dict_correct(word, res, APP_DICT_MAX_RES);
        s_sugg_count = n;
        for (int i = 0; i < n && i < 3; i++) s_sugg_words[i] = res[i].word;
    }

    kbd_apply_suggestion_ui();
}

/* Tap a suggestion → replace the current word with it. */
static void kbd_replace_word(const char *repl)
{
    if (!s_textarea || !repl) return;
    char word[APP_DICT_MAX_WORD + 1];
    kbd_current_word(word, sizeof(word));
    int wlen = (int)strlen(word);
    for (int i = 0; i < wlen; i++) lv_textarea_delete_char(s_textarea);
    lv_textarea_add_text(s_textarea, repl);
    kbd_clear_suggestions();
}

/* If the current word differs from the first suggestion, replace it with
 * the suggestion (called when committing with space / enter). */
static void kbd_auto_accept(void)
{
    if (!s_textarea || s_sugg_count == 0 || !s_sugg_words[0]) return;
    char word[APP_DICT_MAX_WORD + 1];
    kbd_current_word(word, sizeof(word));
    if (word[0] == '\0') return;
    if (strcmp(word, s_sugg_words[0]) == 0) return;
    kbd_replace_word(s_sugg_words[0]);
}

static void kbd_sugg_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_sugg_count || !s_sugg_words[idx]) return;
    app_events_request_vibe_click();
    kbd_replace_word(s_sugg_words[idx]);
}

/* Refresh the auto-correct highlight while the textarea scrolls (long
 * input) so the box stays glued to the real position of the current word. */
static void kbd_ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_BEGIN ||
        code == LV_EVENT_SCROLL_END) {
        s_ta_event = true;
        kbd_update_highlight();
        s_ta_event = false;
    }
}

/* ====================================================================
 * Apple-style key popup
 * ==================================================================== */
static void kbd_popup_init(void)
{
    s_pop.pop = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(s_pop.pop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_pop.pop, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pop.pop, 0, 0);
    lv_obj_set_style_pad_all(s_pop.pop, 0, 0);
    lv_obj_add_flag(s_pop.pop, LV_OBJ_FLAG_HIDDEN);

    s_pop.head = lv_obj_create(s_pop.pop);
    lv_obj_remove_flag(s_pop.head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_pop.head, 10, 0);
    lv_obj_set_style_bg_color(s_pop.head, KBD_COLOR_POPUP, 0);
    lv_obj_set_style_bg_opa(s_pop.head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pop.head, 0, 0);
    lv_obj_set_style_pad_all(s_pop.head, 0, 0);
    lv_obj_align(s_pop.head, LV_ALIGN_TOP_MID, 0, 0);

    s_pop.stem = lv_obj_create(s_pop.pop);
    lv_obj_remove_flag(s_pop.stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_pop.stem, 3, 0);
    lv_obj_set_style_bg_color(s_pop.stem, KBD_COLOR_POPUP, 0);
    lv_obj_set_style_bg_opa(s_pop.stem, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pop.stem, 0, 0);
    lv_obj_align(s_pop.stem, LV_ALIGN_BOTTOM_MID, 0, 0);

    s_pop.label = lv_label_create(s_pop.head);
    lv_obj_set_style_text_font(s_pop.label, KBD_FONT_XXL, 0);
    lv_obj_set_style_text_color(s_pop.label, KBD_COLOR_TEXT, 0);
    lv_obj_center(s_pop.label);
}

static void kbd_popup_show(lv_obj_t *key, const char *ch)
{
    if (!s_pop.pop) return;
    lv_label_set_text(s_pop.label, ch);

    lv_area_t ka;
    lv_obj_get_coords(key, &ka);
    int key_w = ka.x2 - ka.x1 + 1;
    int key_h = ka.y2 - ka.y1 + 1;

    int pop_w = key_w + 34;
    if (pop_w < 62) pop_w = 62;
    int head_h = key_h + 22;
    int stem_h = 14;

    lv_obj_set_width(s_pop.pop, pop_w);
    lv_obj_set_height(s_pop.pop, head_h + stem_h);
    lv_obj_set_width(s_pop.head, pop_w);
    lv_obj_set_height(s_pop.head, head_h);
    lv_obj_set_width(s_pop.stem, pop_w / 2);
    lv_obj_set_height(s_pop.stem, stem_h);

    int cx = (ka.x1 + ka.x2) / 2;
    int x = cx - pop_w / 2;
    if (x < 2) x = 2;
    if (x > KBD_W - pop_w - 2) x = KBD_W - pop_w - 2;
    lv_obj_set_pos(s_pop.pop, x, ka.y1 - (head_h + stem_h) + 2);
    lv_obj_move_foreground(s_pop.pop);
    lv_obj_remove_flag(s_pop.pop, LV_OBJ_FLAG_HIDDEN);
    s_pop.host = key;
}

static void kbd_popup_follow(lv_indev_t *indev)
{
    if (!s_pop.pop || lv_obj_has_flag(s_pop.pop, LV_OBJ_FLAG_HIDDEN)) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    int pop_w = lv_obj_get_width(s_pop.pop);
    int x = pt.x - pop_w / 2;
    if (x < 2) x = 2;
    if (x > KBD_W - pop_w - 2) x = KBD_W - pop_w - 2;
    lv_obj_set_x(s_pop.pop, x);
}

static void kbd_popup_hide(void)
{
    if (s_pop.pop) lv_obj_add_flag(s_pop.pop, LV_OBJ_FLAG_HIDDEN);
    s_pop.host = NULL;
}

/* ====================================================================
 * Key construction
 * ==================================================================== */

/* Label text: applies caps to letters and the 123/ABC toggle label. */
static const char *kbd_key_label(const kbd_key_def_t *def)
{
    if (def->type == KBD_KEY_MODE) return s_sym_mode ? "ABC" : "123";
    if (def->type == KBD_KEY_CHAR && !def->icon && s_caps &&
        def->label[0] >= 'a' && def->label[0] <= 'z') {
        static char up[2] = { 0, 0 };
        up[0] = (char)(def->label[0] - 32);
        return up;
    }
    return def->label;
}

static void kbd_refresh_key_labels(void)
{
    if (!s_keys_layer) return;
    uint32_t nkeys = lv_obj_get_child_count(s_keys_layer);
    for (uint32_t i = 0; i < nkeys; i++) {
        lv_obj_t *key = lv_obj_get_child(s_keys_layer, i);
        if (!key) continue;
        kbd_key_def_t *def = lv_obj_get_user_data(key);
        if (!def || def->icon) continue;
        lv_obj_t *lbl = lv_obj_get_child(key, 0);
        if (!lbl) continue;

        if (def->type == KBD_KEY_MODE) {
            lv_label_set_text(lbl, s_sym_mode ? "ABC" : "123");
        } else if (def->type == KBD_KEY_CHAR) {
            if (s_caps && def->label[0] >= 'a' && def->label[0] <= 'z') {
                char up[2] = { (char)(def->label[0] - 32), 0 };
                lv_label_set_text(lbl, up);
            } else {
                lv_label_set_text(lbl, def->label);
            }
        }
    }
}

static void kbd_key_activate(const kbd_key_def_t *def)
{
    switch (def->type) {
    case KBD_KEY_CHAR:
        kbd_insert_char(def->label[0]);
        break;
    case KBD_KEY_CAPS:
        s_caps = !s_caps;
        kbd_refresh_key_labels();
        break;
    case KBD_KEY_MODE:
        s_sym_mode = !s_sym_mode;
        kbd_build_keys();
        break;
    case KBD_KEY_SPACE:
        kbd_insert_char(' ');
        break;
    case KBD_KEY_RETURN:
        kbd_finish();
        break;
    case KBD_KEY_EMOJI:
        /* Reserved — emoji palette comes later. */
        break;
    }
}

static void kbd_key_event_cb(lv_event_t *e)
{
    lv_obj_t *key = lv_event_get_target(e);
    kbd_key_def_t *def = lv_obj_get_user_data(key);
    lv_event_code_t code = lv_event_get_code(e);
    if (!def) return;

    switch (code) {
    case LV_EVENT_PRESSED:
        /* Haptic press feedback (respects Silent Mode via app_events).
         * The pressed key also turns grey (LV_STATE_PRESSED style). */
        app_events_request_vibe_click();
        if (def->type == KBD_KEY_CHAR) kbd_popup_show(key, kbd_key_label(def));
        break;
    case LV_EVENT_PRESSING:
        if (def->type == KBD_KEY_CHAR) kbd_popup_follow(lv_indev_active());
        break;
    case LV_EVENT_CLICKED:
        kbd_popup_hide();
        kbd_key_activate(def);
        break;
    case LV_EVENT_PRESS_LOST:
    case LV_EVENT_RELEASED:
        kbd_popup_hide();
        break;
    default:
        break;
    }
}

static void kbd_create_key(const kbd_key_def_t *def, int x, int y, int w, int h)
{
    lv_obj_t *key = lv_button_create(s_keys_layer);
    lv_obj_set_pos(key, x, y);
    lv_obj_set_size(key, w, h);
    lv_obj_set_style_radius(key, 5, 0);
    lv_obj_set_style_bg_color(key, KBD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    /* The key turns grey while pressed. */
    lv_obj_set_style_bg_color(key, KBD_COLOR_KEY_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(key, 0, 0);
    lv_obj_set_style_shadow_width(key, 0, 0);
    lv_obj_remove_flag(key, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(key, (void *)def);

    if (def->icon) {
        const lv_image_dsc_t *src = (def->type == KBD_KEY_CAPS)
            ? &kbd_icon_caps : &kbd_icon_emoji;
        lv_obj_t *img = lv_image_create(key);
        lv_image_set_src(img, src);
        lv_obj_center(img);
        lv_image_set_scale(img, (def->type == KBD_KEY_CAPS) ? 120 : 90);
    } else {
        lv_obj_t *lbl = lv_label_create(key);
        lv_label_set_text(lbl, kbd_key_label(def));
        lv_obj_center(lbl);
        lv_obj_set_style_text_color(lbl, KBD_COLOR_TEXT, 0);
        if (def->type == KBD_KEY_MODE) {
            /* The "123"/"ABC" key keeps the small outlined label. */
            lv_obj_set_style_text_font(lbl, KBD_FONT_S, 0);
            lv_obj_set_style_outline_color(lbl, KBD_COLOR_TEXT, 0);
            lv_obj_set_style_outline_opa(lbl, LV_OPA_COVER, 0);
            lv_obj_set_style_outline_width(lbl, 2, 0);
            lv_obj_set_style_outline_pad(lbl, 2, 0);
        } else {
            lv_obj_set_style_text_font(lbl, KBD_FONT_M, 0);
        }
    }

    lv_obj_add_event_cb(key, kbd_key_event_cb, LV_EVENT_ALL, NULL);
}

/* Build all rows.  The q–p (and 1..0) row scales to fill 410 exactly; every
 * other row uses the same ratio (KBD_SCALE) and is centred horizontally. */
static void kbd_build_keys(void)
{
    if (!s_keys_layer) {
        /* Full-screen transparent, NON-clickable layer — it holds the key
         * buttons but lets touches pass through to Cancel / Done and the
         * backspace button beneath it. */
        s_keys_layer = lv_obj_create(s_scr);
        lv_obj_set_size(s_keys_layer, KBD_W, KBD_H);
        lv_obj_set_style_bg_opa(s_keys_layer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_keys_layer, 0, 0);
        lv_obj_remove_flag(s_keys_layer,
                           LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clean(s_keys_layer);
    }

    const kbd_row_t *rows = s_sym_mode ? kbd_rows_sym : kbd_rows_alpha;
    const int nrows = 4;

    for (int r = 0; r < nrows; r++) {
        const kbd_row_t *row = &rows[r];
        int widths[10];
        int total = 0;

        if (row->fill) {
            /* Fill the full width — the last key takes the remainder. */
            double scale = KBD_SCALE;
            int x = 0;
            for (int c = 0; c < row->count; c++) {
                const kbd_key_def_t *def = &row->keys[c];
                int w = (c == row->count - 1)
                    ? (KBD_W - x)
                    : (int)(def->w * scale);
                kbd_create_key(def, x, row->y, w, def->h);
                x += w;
            }
            continue;
        }

        /* Same ratio as q–p, then centre the row. */
        for (int c = 0; c < row->count; c++) {
            widths[c] = (int)(row->keys[c].w * KBD_SCALE);
            total += widths[c];
        }
        int x = (KBD_W - total) / 2;
        if (x < 0) x = 0;
        for (int c = 0; c < row->count; c++) {
            kbd_create_key(&row->keys[c], x, row->y, widths[c], row->keys[c].h);
            x += widths[c];
        }
    }
}

/* ====================================================================
 * Screen construction
 * ==================================================================== */
static void kbd_build_top(void)
{
    /* Cancel (left) — rounded 50, like the original */
    lv_obj_t *cancel = lv_button_create(s_scr);
    lv_obj_set_size(cancel, 127, 62);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 26, 33);
    lv_obj_set_style_radius(cancel, 50, 0);
    lv_obj_set_style_bg_color(cancel, KBD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cancel, KBD_COLOR_KEY_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_remove_flag(cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_font(cl, KBD_FONT_L, 0);
    lv_obj_set_style_text_color(cl, KBD_COLOR_TEXT, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, kbd_cancel_cb, LV_EVENT_CLICKED, NULL);

    /* Done (right) — blue text, like the original */
    lv_obj_t *done = lv_button_create(s_scr);
    lv_obj_set_size(done, 127, 62);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, -26, 33);
    lv_obj_set_style_radius(done, 50, 0);
    lv_obj_set_style_bg_color(done, KBD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(done, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(done, KBD_COLOR_KEY_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(done, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(done, 0, 0);
    lv_obj_set_style_shadow_width(done, 0, 0);
    lv_obj_remove_flag(done, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "Done");
    lv_obj_set_style_text_font(dl, KBD_FONT_L, 0);
    lv_obj_set_style_text_color(dl, KBD_COLOR_BLUE, 0);
    lv_obj_center(dl);
    lv_obj_add_event_cb(done, kbd_done_cb, LV_EVENT_CLICKED, NULL);
}

/* Input panel (textarea + backspace button) — spans y 174..224. */
static void kbd_build_input(void)
{
    lv_obj_t *panel = lv_obj_create(s_scr);
    lv_obj_set_size(panel, KBD_W, 50);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -(KBD_PANEL2_H - KBD_DOWN_SHIFT));
    lv_obj_set_style_bg_color(panel, KBD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Inset the content so the backspace button clears the round corner. */
    lv_obj_set_style_pad_left(panel, 20, 0);
    lv_obj_set_style_pad_right(panel, 28, 0);
    s_input_panel = panel;

    /* Blue highlight box for the word being auto-corrected — a rounded
     * blue box on the panel behind the (transparent) textarea. */
    s_word_hl = lv_obj_create(panel);
    lv_obj_set_style_radius(s_word_hl, 0, 0);
    lv_obj_set_style_bg_color(s_word_hl, KBD_COLOR_BLUE, 0);
    lv_obj_set_style_bg_opa(s_word_hl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_word_hl, 0, 0);
    lv_obj_set_style_shadow_width(s_word_hl, 0, 0);
    lv_obj_remove_flag(s_word_hl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_word_hl, LV_OBJ_FLAG_HIDDEN);

    s_textarea = lv_textarea_create(panel);
    lv_obj_set_size(s_textarea, 250, 50);
    lv_obj_set_align(s_textarea, LV_ALIGN_LEFT_MID);
    lv_obj_set_x(s_textarea, 16);
    lv_textarea_set_placeholder_text(s_textarea, "Enter...");
    lv_textarea_set_one_line(s_textarea, true);
    lv_obj_set_scrollbar_mode(s_textarea, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_color(s_textarea, KBD_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_textarea, KBD_FONT_TX, 0);
    /* Transparent background so the highlight box shows through behind the
     * current word; the text is vertically centred via equal top/bottom
     * padding (pads match kbd_update_highlight's geometry). */
    lv_obj_set_style_bg_color(s_textarea, KBD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_textarea, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_textarea, 0, 0);
    lv_obj_set_style_pad_all(s_textarea, 0, 0);
    lv_obj_set_style_pad_left(s_textarea, 4, 0);
    lv_obj_set_style_pad_right(s_textarea, 4, 0);
    lv_obj_set_style_pad_top(s_textarea,
        (50 - (lv_coord_t)lv_font_get_line_height(KBD_FONT_TX)) / 2, 0);
    lv_obj_set_style_pad_bottom(s_textarea,
        (50 - (lv_coord_t)lv_font_get_line_height(KBD_FONT_TX)) / 2, 0);

    /* Re-track the auto-correct highlight on every scroll step so it stays
     * under the current word even when the input is long and scrolled. */
    lv_obj_add_event_cb(s_textarea, kbd_ta_event_cb, LV_EVENT_ALL, NULL);
    /* Thin white blinking cursor (the theme only shows one when focused;
     * this textarea is never tapped, so style the cursor directly). */
    lv_obj_set_style_bg_opa(s_textarea, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_color(s_textarea, KBD_COLOR_TEXT, LV_PART_CURSOR);
    lv_obj_set_style_border_width(s_textarea, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(s_textarea, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_pad_left(s_textarea, -1, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(s_textarea, 530, LV_PART_CURSOR);

    /* Backspace button (right) with the backspace icon */
    lv_obj_t *bs = lv_button_create(panel);
    lv_obj_set_size(bs, 85, 50);
    lv_obj_align(bs, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(bs, 10, 0);
    lv_obj_set_style_bg_color(bs, KBD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(bs, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bs, KBD_COLOR_KEY_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(bs, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(bs, 0, 0);
    lv_obj_set_style_shadow_width(bs, 0, 0);
    lv_obj_remove_flag(bs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *img = lv_image_create(bs);
    lv_image_set_src(img, &kbd_icon_backspace);
    lv_obj_center(img);
    lv_image_set_scale(img, 175);
    lv_obj_add_event_cb(bs, kbd_backspace_cb, LV_EVENT_CLICKED, NULL);
}

/* Suggestion bar — up to 3 auto-correct slots with "|" separators.
 * The layout adapts to the suggestion count: 1 → single centred slot,
 * 2 → two slots either side of a centred "|", 3 → the full original
 * three-slot layout.  Spans y 224..274 (shifted down with the keyboard). */
static void kbd_build_sugg(void)
{
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_set_size(bar, KBD_W, 50);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, KBD_PANEL2_TOP);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        s_sugg[i] = lv_label_create(bar);
        lv_obj_set_width(s_sugg[i], 120);
        /* Single-line text, centred and clipped — no circular scroll that
         * shifts the label off-centre when the word overflows. */
        lv_label_set_long_mode(s_sugg[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(s_sugg[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(s_sugg[i], "");
        lv_obj_set_style_text_font(s_sugg[i], KBD_FONT_M, 0);
        lv_obj_set_style_text_color(s_sugg[i], KBD_COLOR_TEXT, 0);
        lv_obj_center(s_sugg[i]);
        lv_obj_add_flag(s_sugg[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_sugg[i], kbd_sugg_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    for (int i = 0; i < 2; i++) {
        s_sep[i] = lv_label_create(bar);
        lv_label_set_text(s_sep[i], "|");
        lv_obj_set_style_text_font(s_sep[i], KBD_FONT_M, 0);
        lv_obj_set_style_text_color(s_sep[i], KBD_COLOR_GREY_SEP, 0);
        lv_obj_add_flag(s_sep[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ====================================================================
 * Open / close / finish
 * ==================================================================== */
static void kbd_teardown(void)
{
    kbd_popup_hide();

    /* The popup lives on the top layer — free it with the keyboard. */
    if (s_pop.pop) { lv_obj_del(s_pop.pop); s_pop = (kbd_popup_t){0}; }

    /* Restore the previous screen first, then free the keyboard screen. */
    lv_obj_t *prev = s_prev_screen;
    s_prev_screen = NULL;
    lv_obj_t *scr = s_scr;
    s_scr = NULL;
    s_textarea = NULL;
    s_keys_layer = NULL;
    memset(s_sugg, 0, sizeof(s_sugg));
    memset(s_sep, 0, sizeof(s_sep));
    s_word_hl = NULL;
    s_input_panel = NULL;
    s_ta_event = false;
    s_sugg_count = 0;
    s_active = false;
    if (prev) lv_screen_load(prev);
    if (scr) lv_obj_del(scr);
}

static void kbd_finish(void)
{
    if (!s_active) return;
    keyboard_done_cb_t cb = s_cb;
    s_cb = NULL;
    /* Commit the top suggestion for the final word before submitting. */
    kbd_auto_accept();
    const char *txt = s_textarea ? lv_textarea_get_text(s_textarea) : "";
    char out[KEYBOARD_MAX_TEXT + 1];
    strncpy(out, txt ? txt : "", KEYBOARD_MAX_TEXT);
    out[KEYBOARD_MAX_TEXT] = '\0';
    kbd_teardown();
    if (cb) cb(out);
}

static void kbd_done_cb(lv_event_t *e)
{
    (void)e;
    kbd_finish();
}

static void kbd_cancel_cb(lv_event_t *e)
{
    (void)e;
    keyboard_close();
}

static void kbd_backspace_cb(lv_event_t *e)
{
    (void)e;
    app_events_request_vibe_click();
    kbd_backspace();
}

void keyboard_open(const char *initial_text, keyboard_done_cb_t cb)
{
    if (s_active) keyboard_close();

    s_active = true;
    s_caps = false;
    s_sym_mode = false;
    s_cb = cb;

    /* Load the dictionary once (from SPIFFS into PSRAM). */
    if (!app_dict_ready()) {
        app_dict_init();
    }

    s_prev_screen = lv_screen_active();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, KBD_W, KBD_H);
    lv_obj_set_style_bg_color(s_scr, KBD_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    kbd_popup_init();
    kbd_build_top();
    kbd_build_input();
    kbd_build_sugg();
    kbd_build_keys();

    if (s_textarea && initial_text) {
        lv_textarea_set_text(s_textarea, initial_text);
    }
    kbd_update_suggestions();

    lv_screen_load(s_scr);
    ESP_LOGI(TAG, "Keyboard open");
}

void keyboard_close(void)
{
    if (!s_active) return;
    keyboard_done_cb_t cb = s_cb;
    s_cb = NULL;
    kbd_teardown();
    if (cb) cb(NULL);
}

bool keyboard_is_active(void)
{
    return s_active;
}
