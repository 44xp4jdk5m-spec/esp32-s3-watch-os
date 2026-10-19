/**
 * @file calculator_ui.c
 * @brief iOS-style calculator app UI (ported from ElenixOS com.elenixos.calculator)
 *
 * Faithful C port of the ElenixOS calculator `main.js`:
 *
 *   https://github.com/ElenixOS/ElenixOS-Simulator/tree/main/fs/.sys/app/apps/com.elenixos.calculator
 *
 * The layout is computed with the exact same percentage math as the reference
 * (pad/gap/cell sizes), so gaps and sizes match the original design. Only the
 * rendering calls are translated from the ElenixOS LVGL JS binding to the
 * LVGL v9 C API.
 *
 * Memory model (OS/app split):
 *   - `s_calc` (all app state) is allocated from PSRAM on create().
 *   - LVGL widgets are allocated by the PSRAM-backed LVGL allocator.
 *   - destroy() deletes every widget and frees `s_calc`, returning all RAM.
 *   - The main OS never references this module until the icon is tapped.
 */

#include "calculator_ui.h"

#include "app_events.h"
#include "fonts.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_QilkaBold42);

static const char *TAG = "CALC";

/* ── Screen size (matches the 410x502 AMOLED display) ──────────────── */
#define CALC_W  410
#define CALC_H  502

/* ── Colour palette (1:1 from ElenixOS main.js) ────────────────────── */
#define CALC_COLOR_BG        0x000000
#define CALC_COLOR_DISPLAY   0x1C1C1E
#define CALC_COLOR_DIGIT     0x2C2C2E
#define CALC_COLOR_FUNC      0xA5A5A5
#define CALC_COLOR_OP        0xFF9F0A
#define CALC_COLOR_TEXT_LIGHT 0xFFFFFF
#define CALC_COLOR_TEXT_DARK  0x000000

/* ── Operator glyphs (Unicode, UTF-8 encoded) ──────────────────────── */
#define CALC_GLYPH_DIVIDE    "\xC3\xB7"      /* U+00F7 division sign   */
#define CALC_GLYPH_MULTIPLY  "\xC3\x97"      /* U+00D7 multiplication  */
#define CALC_GLYPH_SUBTRACT  "\xE2\x88\x92"  /* U+2212 minus sign       */
#define CALC_GLYPH_BACKSPACE "<"  /* "<" replaces U+232B (glyph missing from font) */

/* ── Calculation limits (1:1 from reference) ───────────────────────── */
#define CALC_MAX_INPUT_LEN  32
#define CALC_MAX_ABS_VALUE  999999999999999999.0
#define CALC_INPUT_BUF      96

/* Pressed-state scale (LVGL transform scale: 256 = 100%) */
#define CALC_SCALE_PRESSED  490

/* ── Key identifiers ────────────────────────────────────────────────── */
typedef enum {
    CALC_KEY_0 = 0,
    CALC_KEY_1,
    CALC_KEY_2,
    CALC_KEY_3,
    CALC_KEY_4,
    CALC_KEY_5,
    CALC_KEY_6,
    CALC_KEY_7,
    CALC_KEY_8,
    CALC_KEY_9,
    CALC_KEY_DOT,
    CALC_KEY_AC,
    CALC_KEY_SIGN,          /* +/- */
    CALC_KEY_PERCENT,
    CALC_KEY_EQUALS,
    CALC_KEY_DIVIDE,
    CALC_KEY_MULTIPLY,
    CALC_KEY_SUBTRACT,
    CALC_KEY_ADD,
} calc_key_t;

typedef enum {
    CALC_TYPE_FUNC,
    CALC_TYPE_DIGIT,
    CALC_TYPE_OP,
} calc_key_type_t;

typedef enum {
    OP_NONE = 0,
    OP_DIV,
    OP_MUL,
    OP_SUB,
    OP_ADD,
} calc_op_t;

/* ── Button layout table (4x5 grid, "0" spans two columns) ─────────── */
typedef struct {
    calc_key_t     key;
    const char    *label;
    calc_key_type_t type;
    uint8_t        row;
    uint8_t        col;
    uint8_t        span;
} calc_layout_t;

static const calc_layout_t s_layout[] = {
    { CALC_KEY_AC,        "AC",                 CALC_TYPE_FUNC,  0, 0, 1 },
    { CALC_KEY_SIGN,      "+/-",                CALC_TYPE_FUNC,  0, 1, 1 },
    { CALC_KEY_PERCENT,   "%",                  CALC_TYPE_FUNC,  0, 2, 1 },
    { CALC_KEY_DIVIDE,    CALC_GLYPH_DIVIDE,    CALC_TYPE_OP,    0, 3, 1 },
    { CALC_KEY_7,         "7",                  CALC_TYPE_DIGIT, 1, 0, 1 },
    { CALC_KEY_8,         "8",                  CALC_TYPE_DIGIT, 1, 1, 1 },
    { CALC_KEY_9,         "9",                  CALC_TYPE_DIGIT, 1, 2, 1 },
    { CALC_KEY_MULTIPLY,  CALC_GLYPH_MULTIPLY,  CALC_TYPE_OP,    1, 3, 1 },
    { CALC_KEY_4,         "4",                  CALC_TYPE_DIGIT, 2, 0, 1 },
    { CALC_KEY_5,         "5",                  CALC_TYPE_DIGIT, 2, 1, 1 },
    { CALC_KEY_6,         "6",                  CALC_TYPE_DIGIT, 2, 2, 1 },
    { CALC_KEY_SUBTRACT,  CALC_GLYPH_SUBTRACT,  CALC_TYPE_OP,    2, 3, 1 },
    { CALC_KEY_1,         "1",                  CALC_TYPE_DIGIT, 3, 0, 1 },
    { CALC_KEY_2,         "2",                  CALC_TYPE_DIGIT, 3, 1, 1 },
    { CALC_KEY_3,         "3",                  CALC_TYPE_DIGIT, 3, 2, 1 },
    { CALC_KEY_ADD,       "+",                  CALC_TYPE_OP,    3, 3, 1 },
    { CALC_KEY_0,         "0",                  CALC_TYPE_DIGIT, 4, 0, 2 },
    { CALC_KEY_DOT,       ".",                  CALC_TYPE_DIGIT, 4, 2, 1 },
    { CALC_KEY_EQUALS,    "=",                  CALC_TYPE_OP,    4, 3, 1 },
};

#define CALC_LAYOUT_COUNT (sizeof(s_layout) / sizeof(s_layout[0]))

/* ── App context (allocated in PSRAM, freed on exit) ───────────────── */
typedef struct {
    lv_obj_t *scr;              /* root screen */
    lv_obj_t *display_label;
    lv_obj_t *delete_btn;
    lv_obj_t *op_buttons[OP_ADD + 1];
    lv_obj_t *op_labels[OP_ADD + 1];

    /* Calculator state */
    char      input[CALC_INPUT_BUF];
    double    stored;
    bool      has_stored;
    calc_op_t pending_op;
    bool      result_shown;
    bool      has_error;
    char      error_text[16];
    calc_op_t active_op;
    bool      display_dimmed;

    /* Layout metrics */
    int pad, gap, display_h;
    int display_wrap_w, display_wrap_h;
    int cell_w, cell_h, radius;
    int keypad_y;
    int display_right_inset;
} calculator_ctx_t;

static calculator_ctx_t *s_calc = NULL;
static lv_obj_t        *s_prev_screen = NULL;   /* screen restored on exit */

/* ── Forward decls ──────────────────────────────────────────────────── */
static void calc_update_display(void);
static void calc_set_display_dimmed(bool dimmed);

/* ── Helpers (1:1 with the JS reference) ───────────────────────────── */

static bool calc_value_overflow(double value)
{
    if (!isfinite(value)) return true;
    return fabs(value) > CALC_MAX_ABS_VALUE;
}

static bool calc_text_overflow(const char *text)
{
    const char *t = text;
    if (t[0] == '-') t++;
    return (int)strlen(t) > CALC_MAX_INPUT_LEN;
}

static bool calc_next_input_allowed(const char *text)
{
    if (calc_text_overflow(text)) return false;
    double n = strtod(text, NULL);
    if (n == 0.0 && strcmp(text, "0") != 0 && strcmp(text, "0.") != 0 &&
        strcmp(text, "-0") != 0) {
        /* strtod failed to parse a number — JS parseFloat would be NaN → allow */
        return true;
    }
    return !calc_value_overflow(n);
}

static int calc_format_number(double value, char *buf, size_t len)
{
    if (!isfinite(value) || fabs(value) > CALC_MAX_ABS_VALUE) return -1;

    /* JS `parseFloat(value.toFixed(10))` — round to 10 decimal places */
    double rounded = floor(fabs(value) * 1e10 + 0.5) / 1e10;
    if (value < 0) rounded = -rounded;

    if (rounded == 0.0) {
        snprintf(buf, len, "0");
    } else {
        snprintf(buf, len, "%.10f", rounded);
        /* trim trailing zeros, then a trailing dot (match JS String()) */
        size_t n = strlen(buf);
        if (strchr(buf, '.') != NULL) {
            while (n > 1 && buf[n - 1] == '0') buf[--n] = '\0';
            if (n > 1 && buf[n - 1] == '.') buf[--n] = '\0';
        }
    }

    if (calc_text_overflow(buf)) return -1;
    return 0;
}

static double calc_get_number(void)
{
    return strtod(s_calc->input, NULL);
}

static bool calc_apply_op(double a, double b, calc_op_t op, double *out)
{
    switch (op) {
    case OP_ADD: *out = a + b; return true;
    case OP_SUB: *out = a - b; return true;
    case OP_MUL: *out = a * b; return true;
    case OP_DIV:
        if (b == 0.0) return false;
        *out = a / b;
        return true;
    default:
        *out = b;
        return true;
    }
}

static calc_op_t calc_key_to_op(calc_key_t key)
{
    switch (key) {
    case CALC_KEY_DIVIDE:   return OP_DIV;
    case CALC_KEY_MULTIPLY: return OP_MUL;
    case CALC_KEY_SUBTRACT: return OP_SUB;
    case CALC_KEY_ADD:      return OP_ADD;
    default:                return OP_NONE;
    }
}

/* ── Operator button highlight ──────────────────────────────────────── */

static void calc_restore_op_button(calc_op_t op)
{
    if (op <= OP_NONE || op > OP_ADD) return;
    lv_obj_t *btn = s_calc->op_buttons[op];
    lv_obj_t *lbl = s_calc->op_labels[op];
    if (btn) lv_obj_set_style_bg_color(btn, lv_color_hex(CALC_COLOR_OP), 0);
    if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(CALC_COLOR_TEXT_LIGHT), 0);
}

static void calc_set_active_op(calc_op_t op)
{
    if (s_calc->active_op != OP_NONE && s_calc->active_op != op) {
        calc_restore_op_button(s_calc->active_op);
    }
    s_calc->active_op = op;

    if (op <= OP_NONE || op > OP_ADD) return;
    lv_obj_t *btn = s_calc->op_buttons[op];
    lv_obj_t *lbl = s_calc->op_labels[op];
    if (btn) lv_obj_set_style_bg_color(btn, lv_color_hex(CALC_COLOR_TEXT_LIGHT), 0);
    if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(CALC_COLOR_OP), 0);
}

static void calc_clear_active_op(void)
{
    if (s_calc->active_op == OP_NONE) return;
    calc_restore_op_button(s_calc->active_op);
    s_calc->active_op = OP_NONE;
}

/* ── State machine (1:1 with the JS reference) ─────────────────────── */

static void calc_reset_all(void)
{
    snprintf(s_calc->input, sizeof(s_calc->input), "0");
    s_calc->stored = 0;
    s_calc->has_stored = false;
    s_calc->pending_op = OP_NONE;
    s_calc->result_shown = false;
    s_calc->has_error = false;
    snprintf(s_calc->error_text, sizeof(s_calc->error_text), "Error");
    calc_clear_active_op();
}

static void calc_set_error(const char *text)
{
    s_calc->has_error = true;
    snprintf(s_calc->error_text, sizeof(s_calc->error_text), "%s",
             text ? text : "Error");
}

static void calc_input_digit(char digit)
{
    if (s_calc->has_error) calc_reset_all();

    if (s_calc->result_shown) {
        char next[2] = { digit, '\0' };
        if (!calc_next_input_allowed(next)) return;
        snprintf(s_calc->input, sizeof(s_calc->input), "%c", digit);
        s_calc->result_shown = false;
        return;
    }

    char next[CALC_INPUT_BUF];
    if (strcmp(s_calc->input, "0") == 0) {
        snprintf(next, sizeof(next), "%c", digit);
    } else {
        snprintf(next, sizeof(next), "%s%c", s_calc->input, digit);
    }
    if (!calc_next_input_allowed(next)) {
        calc_set_error("Overflow");
        return;
    }
    snprintf(s_calc->input, sizeof(s_calc->input), "%s", next);
}

static void calc_input_dot(void)
{
    if (s_calc->has_error) calc_reset_all();

    if (s_calc->result_shown) {
        if (!calc_next_input_allowed("0.")) return;
        snprintf(s_calc->input, sizeof(s_calc->input), "0.");
        s_calc->result_shown = false;
        return;
    }

    if (strchr(s_calc->input, '.') != NULL) return;

    char next[CALC_INPUT_BUF];
    snprintf(next, sizeof(next), "%s.", s_calc->input);
    if (!calc_next_input_allowed(next)) {
        calc_set_error("Overflow");
        return;
    }
    snprintf(s_calc->input, sizeof(s_calc->input), "%s", next);
}

static void calc_toggle_sign(void)
{
    if (s_calc->has_error) return;
    if (strcmp(s_calc->input, "0") == 0) return;

    if (s_calc->input[0] == '-') {
        size_t len = strlen(s_calc->input);
        memmove(s_calc->input, s_calc->input + 1, len); /* len includes '\0' */
    } else {
        char next[CALC_INPUT_BUF];
        snprintf(next, sizeof(next), "-%s", s_calc->input);
        if (!calc_next_input_allowed(next)) return;
        snprintf(s_calc->input, sizeof(s_calc->input), "%s", next);
    }
}

static void calc_to_percent(void)
{
    if (s_calc->has_error) return;

    double value = calc_get_number() / 100.0;
    char text[CALC_INPUT_BUF];
    if (calc_format_number(value, text, sizeof(text)) != 0) {
        calc_set_error("Overflow");
        return;
    }
    snprintf(s_calc->input, sizeof(s_calc->input), "%s", text);
    s_calc->result_shown = true;
}

static void calc_press_operator(calc_op_t op)
{
    if (s_calc->has_error) return;

    double current = calc_get_number();

    if (!s_calc->has_stored) {
        s_calc->stored = current;
        s_calc->has_stored = true;
    } else if (s_calc->pending_op != OP_NONE && !s_calc->result_shown) {
        double result;
        if (!calc_apply_op(s_calc->stored, current, s_calc->pending_op, &result)) {
            calc_set_error("Error");
            return;
        }
        char text[CALC_INPUT_BUF];
        if (calc_format_number(result, text, sizeof(text)) != 0) {
            calc_set_error("Overflow");
            return;
        }
        s_calc->stored = result;
        snprintf(s_calc->input, sizeof(s_calc->input), "%s", text);
    }

    calc_set_active_op(op);
    s_calc->pending_op = op;
    s_calc->result_shown = true;
}

static void calc_press_equal(void)
{
    if (s_calc->has_error) return;
    if (s_calc->pending_op == OP_NONE || !s_calc->has_stored) return;

    double current = calc_get_number();
    double result;
    if (!calc_apply_op(s_calc->stored, current, s_calc->pending_op, &result)) {
        calc_set_error("Error");
        calc_update_display();
        return;
    }

    char text[CALC_INPUT_BUF];
    if (calc_format_number(result, text, sizeof(text)) != 0) {
        calc_set_error("Overflow");
        calc_update_display();
        return;
    }

    calc_clear_active_op();
    snprintf(s_calc->input, sizeof(s_calc->input), "%s", text);
    s_calc->has_stored = false;
    s_calc->stored = 0;
    s_calc->pending_op = OP_NONE;
    s_calc->result_shown = true;
}

static void calc_backspace(void)
{
    if (s_calc->has_error) {
        calc_reset_all();
        return;
    }

    if (s_calc->result_shown) s_calc->result_shown = false;

    size_t n = strlen(s_calc->input);
    if (n <= 1) {
        snprintf(s_calc->input, sizeof(s_calc->input), "0");
        return;
    }
    if (n == 2 && s_calc->input[0] == '-') {
        snprintf(s_calc->input, sizeof(s_calc->input), "0");
        return;
    }
    s_calc->input[n - 1] = '\0';
}

static void calc_handle_key(calc_key_t key)
{
    if (key <= CALC_KEY_9) {
        calc_input_digit((char)('0' + (int)key));
    } else if (key == CALC_KEY_DOT) {
        calc_input_dot();
    } else if (key == CALC_KEY_AC) {
        calc_reset_all();
    } else if (key == CALC_KEY_SIGN) {
        calc_toggle_sign();
    } else if (key == CALC_KEY_PERCENT) {
        calc_to_percent();
    } else if (key == CALC_KEY_EQUALS) {
        calc_press_equal();
    } else {
        calc_press_operator(calc_key_to_op(key));
    }
    calc_update_display();
}

/* ── Display ────────────────────────────────────────────────────────── */

static void calc_set_display_dimmed(bool dimmed)
{
    if (!s_calc || s_calc->display_dimmed == dimmed) return;
    s_calc->display_dimmed = dimmed;

    lv_obj_set_style_translate_x(s_calc->display_label, dimmed ? 20 : 0, 0);

    if (!s_calc->delete_btn) return;
    if (dimmed) {
        lv_obj_remove_flag(s_calc->delete_btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(s_calc->delete_btn, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_opa(s_calc->delete_btn, dimmed ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

static void calc_update_display(void)
{
    if (!s_calc || !s_calc->display_label) return;

    lv_label_set_text(s_calc->display_label,
                      s_calc->has_error ? s_calc->error_text : s_calc->input);

    bool should_dim = !s_calc->has_error &&
                      (strcmp(s_calc->input, "0") == 0 ||
                       (s_calc->result_shown && s_calc->pending_op != OP_NONE));
    calc_set_display_dimmed(should_dim);
}

/* ── Event callbacks ────────────────────────────────────────────────── */

static void calc_key_cb(lv_event_t *e)
{
    calc_key_t key = (calc_key_t)(intptr_t)lv_event_get_user_data(e);
    calc_handle_key(key);
}

static void calc_pressed_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (btn) lv_obj_move_foreground(btn);
}

static void calc_backspace_cb(lv_event_t *e)
{
    (void)e;
    calc_backspace();
    calc_update_display();
}

/* ── UI construction ────────────────────────────────────────────────── */

static void calc_style_button(lv_obj_t *btn, calc_key_type_t type, int radius)
{
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    switch (type) {
    case CALC_TYPE_DIGIT:
        lv_obj_set_style_bg_color(btn, lv_color_hex(CALC_COLOR_DIGIT), 0);
        break;
    case CALC_TYPE_FUNC:
        lv_obj_set_style_bg_color(btn, lv_color_hex(CALC_COLOR_FUNC), 0);
        break;
    default:
        lv_obj_set_style_bg_color(btn, lv_color_hex(CALC_COLOR_OP), 0);
        break;
    }
}

static void calc_style_button_text(lv_obj_t *label, calc_key_type_t type)
{
    lv_obj_set_style_text_color(label,
        lv_color_hex(type == CALC_TYPE_FUNC ? CALC_COLOR_TEXT_DARK
                                            : CALC_COLOR_TEXT_LIGHT), 0);
}

static void calc_create_key(lv_obj_t *root, const calc_layout_t *item)
{
    calculator_ctx_t *c = s_calc;
    int span = item->span ? item->span : 1;
    int x = c->pad + item->col * (c->cell_w + c->gap);
    int y = c->keypad_y + item->row * (c->cell_h + c->gap);
    int w = c->cell_w * span + c->gap * (span - 1);

    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, c->cell_h);
    calc_style_button(btn, item->type, c->radius);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, item->label);
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, &noto_font_24, 0);
    calc_style_button_text(label, item->type);

    if (item->type == CALC_TYPE_OP) {
        calc_op_t op = calc_key_to_op(item->key);
        if (op > OP_NONE && op <= OP_ADD) {
            c->op_buttons[op] = btn;
            c->op_labels[op] = label;
        }
    }

    /* Pressed: scale up (animated) + rise to foreground */
    lv_obj_set_style_transform_scale(btn, CALC_SCALE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_anim_duration(btn, 120, 0);

    int piv_x = w / 2;
    int piv_y = c->cell_h / 2;
    if (item->col == 0) {
        piv_x = 0;
    } else if (item->col + item->span >= 4) {
        piv_x = w;
    }
    if (item->row == 0) {
        piv_y = 0;
    } else if (item->row == 4) {
        piv_y = c->cell_h;
    }
    lv_obj_set_style_transform_pivot_x(btn, piv_x, 0);
    lv_obj_set_style_transform_pivot_y(btn, piv_y, 0);

    lv_obj_add_event_cb(btn, calc_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, calc_key_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)item->key);
}

static void calc_build_ui(void)
{
    calculator_ctx_t *c = s_calc;
    const int sw = CALC_W;
    const int sh = CALC_H;

    /* Layout math — identical to the reference JS */
    c->pad = (sw * 4) / 100;
    if (c->pad < 8) c->pad = 8;                        /* max(8, floor(sw*0.04)) */
    c->gap = (sw * 2) / 100;
    if (c->gap < 6) c->gap = 6;                        /* max(6, floor(sw*0.02)) */
    c->display_h = (sh * 28) / 100;                    /* floor(sh*0.28) */

    int dw = sw - c->pad * 2;
    int dh = c->display_h - c->pad;
    c->display_wrap_w = dw;
    c->display_wrap_h = dh;

    int keypad_h = sh - c->display_h - c->pad - c->pad;
    c->keypad_y = c->display_h + c->pad;
    c->cell_w = (sw - c->pad * 2 - c->gap * 3) / 4;
    c->cell_h = (keypad_h - c->gap * 4) / 5;
    int min_cell = c->cell_w < c->cell_h ? c->cell_w : c->cell_h;
    c->radius = min_cell / 2;

    /* ── Root screen ─────────────────────────────────────────── */
    c->scr = lv_obj_create(NULL);
    lv_obj_set_size(c->scr, sw, sh);
    lv_obj_set_style_bg_color(c->scr, lv_color_hex(CALC_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(c->scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c->scr, 0, 0);
    lv_obj_set_style_radius(c->scr, 0, 0);
    lv_obj_set_style_pad_all(c->scr, 0, 0);
    lv_obj_remove_flag(c->scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Display panel (dark rounded rect) ───────────────────── */
    lv_obj_t *display_wrap = lv_obj_create(c->scr);
    lv_obj_set_pos(display_wrap, c->pad, c->pad);
    lv_obj_set_size(display_wrap, dw, dh);
    lv_obj_set_style_bg_color(display_wrap, lv_color_hex(CALC_COLOR_DISPLAY), 0);
    lv_obj_set_style_bg_opa(display_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(display_wrap, 0, 0);
    lv_obj_set_style_radius(display_wrap, dh / 3, 0);
    lv_obj_set_style_pad_all(display_wrap, 0, 0);

    /* ── Backspace (delete) button — hidden while dimmed ─────── */
    int delete_btn_size = (dh * 46) / 100;
    if (delete_btn_size < 28) delete_btn_size = 28;     /* max(28, floor(dh*0.46)) */

    lv_obj_t *delete_btn = lv_button_create(display_wrap);
    lv_obj_set_size(delete_btn, delete_btn_size, delete_btn_size);
    lv_obj_align(delete_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_radius(delete_btn, delete_btn_size / 2, 0);
    lv_obj_set_style_border_width(delete_btn, 0, 0);
    lv_obj_set_style_shadow_width(delete_btn, 0, 0);
    lv_obj_set_style_bg_opa(delete_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_anim_duration(delete_btn, 180, 0);
    lv_obj_set_style_opa(delete_btn, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(delete_btn, LV_OBJ_FLAG_CLICKABLE);
    c->delete_btn = delete_btn;

    lv_obj_t *delete_label = lv_label_create(delete_btn);
    lv_label_set_text(delete_label, CALC_GLYPH_BACKSPACE);
    lv_obj_center(delete_label);
    lv_obj_set_style_text_font(delete_label, &noto_font_24, 0);
    lv_obj_set_style_text_color(delete_label, lv_color_hex(CALC_COLOR_TEXT_LIGHT), 0);
    lv_obj_add_event_cb(delete_btn, calc_backspace_cb, LV_EVENT_CLICKED, NULL);

    c->display_right_inset = delete_btn_size + 18;

    /* ── Display value label (right-aligned, bold) ───────────── */
    lv_obj_t *display_label = lv_label_create(display_wrap);
    lv_obj_set_width(display_label, dw - c->display_right_inset - 8);
    lv_obj_align(display_label, LV_ALIGN_RIGHT_MID, -c->display_right_inset, 0);
    lv_obj_set_style_text_color(display_label, lv_color_hex(CALC_COLOR_TEXT_LIGHT), 0);
    lv_obj_set_style_text_align(display_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(display_label, &ui_font_QilkaBold42, 0);
    lv_label_set_text(display_label, "0");
    lv_obj_set_style_translate_x(display_label, 20, 0);   /* initial dimmed state */
    c->display_label = display_label;

    /* ── Keypad buttons ──────────────────────────────────────── */
    for (size_t i = 0; i < CALC_LAYOUT_COUNT; i++) {
        calc_create_key(c->scr, &s_layout[i]);
    }

    ESP_LOGI(TAG, "Layout: pad=%d gap=%d cell=%dx%d radius=%d keys=%d",
             c->pad, c->gap, c->cell_w, c->cell_h, c->radius,
             (int)CALC_LAYOUT_COUNT);
}

/* ── Public API ─────────────────────────────────────────────────────── */

void calculator_ui_create(void)
{
    if (s_calc) {
        calculator_ui_destroy();
    }

    /* Remember the screen to restore when the app exits (GPIO0). */
    s_prev_screen = lv_screen_active();

    s_calc = (calculator_ctx_t *)heap_caps_calloc(1, sizeof(calculator_ctx_t),
                                                  MALLOC_CAP_SPIRAM);
    if (!s_calc) {
        ESP_LOGE(TAG, "OOM: failed to allocate calculator context in PSRAM");
        return;
    }

    /* Initial calculator state */
    snprintf(s_calc->input, sizeof(s_calc->input), "0");
    s_calc->pending_op = OP_NONE;
    s_calc->active_op = OP_NONE;
    s_calc->display_dimmed = true;

    calc_build_ui();
    lv_screen_load(s_calc->scr);
    calc_update_display();

    ESP_LOGI(TAG, "Calculator UI created");
}

void calculator_ui_destroy(void)
{
    if (!s_calc) return;

    /* Restore the previous screen and let LVGL delete the calculator
     * screen after the fade.  Deleting the active screen directly leaves
     * the display with no active screen, which breaks GPIO0 exit. */
    if (s_prev_screen && lv_obj_is_valid(s_prev_screen)) {
        lv_screen_load_anim(s_prev_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, true);
    } else if (s_calc->scr) {
        lv_obj_delete(s_calc->scr);
    }
    s_calc->scr = NULL;

    heap_caps_free(s_calc);
    s_calc = NULL;
    s_prev_screen = NULL;
    ESP_LOGI(TAG, "Calculator UI destroyed (PSRAM released)");
}

bool calculator_ui_is_active(void)
{
    return s_calc != NULL;
}

void calculator_ui_handle_back(void)
{
    calculator_ui_destroy();
}
