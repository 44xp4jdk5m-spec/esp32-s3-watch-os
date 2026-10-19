/**
 * @file list_view_menu.c
 * @brief Vertical barrel / fisheye app-list launcher (MENU_STYLE_LIST).
 *
 * Every honeycomb icon (all 47, decorative included) is shown as one ROW,
 * in the same order as the honeycomb menu, plus any SD-card JS app rows
 * appended at the end:
 *
 *      [ big circular app icon ]  [ app name (Noto Sans 48) ]
 *
 * The row nearest the screen's vertical centre is full-size; rows above and
 * below scale down with the distance from centre (icon falloff is gentle,
 * the text falloff is faster — mirroring the SquareLine tmp/list_view
 * reference), and spacing compresses only mildly toward the centre.  There
 * is NO dimming.  Rows snap to centre on release.
 *
 * Snapped look (from tmp/list_view, 96px RGB565A8 icons, 410x502):
 *   centre row icon ≈ 103 px (zoom 275/256), ±1 row ≈ 99 px (265/256),
 *   row 2 below ≈ 82 px (220/256); text ≈ 48 px centre, 38 px at ±1 row,
 *   30 px at row 2.  Row pitch ≈ 123 px.  This is what the fisheye below
 *   reproduces (iconMag / labelMag / push coefficients at the top).
 *
 * Perf notes (ESP32-S3, no FPU, LVGL heap = PSRAM via lv_mem_custom.c):
 *  - Icons are scaled from their native source with lv_image_set_scale
 *    (only the ~centre row ever exceeds native, by ≤ ~7%).
 *  - The app name is rasterized ONCE to a white-on-black RGB565 image
 *    (Noto Sans 48) per row, then scaled per frame like a pressed button —
 *    never a different font size.
 *  - Scrolling is a manual camera model (no LVGL native scroll).
 */

#include "list_view_menu.h"
#include "png_images_circle.h"
#include "js_app_registry.h"
#include "app_events.h"
#include "app_config.h"
#include "app_lang.h"
#include "noto_font_48_bold.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "list_view";

/*============================================================================
 * Display / geometry (410x502 panel)
 *============================================================================*/
#ifndef LCD_H_RES
#define LCD_H_RES           410
#endif
#ifndef LCD_V_RES
#define LCD_V_RES           502
#endif

#define LIST_CY             (LCD_V_RES >> 1)     /* 251 screen centre Y */

/* Row pitch (content spacing between consecutive row centres) — from demo. */
#define ROW_PITCH           123
#define ROW_H               ROW_PITCH

/* When the list is snapped (at rest) the spacing between two neighbouring
 * rows is solved so the SHORTEST distance between their two icon
 * circumferences stays constant (GAP_EDGE), regardless of how much each
 * icon is scaled.  Motion frames use the uniform ROW_PITCH and converge to
 * this on settle. */
#define GAP_EDGE            20

/* Fisheye normalisation: t = |dy| / LIST_CY, clamped to 1. */
#define T_NORM              ((int32_t)LIST_CY)

/* Hide rows whose RAW centre is farther than this from the screen centre. */
#define VIS_RANGE           300

/* Icon geometry.  Native icons are 96x96; at centre the icon is shown at
 * 96 * 1.07 ≈ 103 px (the demo's zoom 275). */
#define ICON_NATIVE         96
#define ICON_MAX_FACTOR     1.07f

/* Row anchors inside the row container: icon centre X and the label's left
 * edge X (like the demo, icon column is on the left ~x65).  A name line is
 * WRAPPED (between words only) once it would exceed TEXT_MAX_W; a single
 * word that is wider is never split — it just stays on one line and may use
 * up to LABEL_MAX_W, which is the most width that fits on the 410 px panel
 * even when the row is X-drifted the full amount. */
#define ICON_X              66
#define TEXT_X              148
#define TEXT_MAX_W          204     /* wrap a name only between words past this */
#define LABEL_MAX_W         232     /* hard width cap = 410-TEXT_X-X_DRIFT_MAX */

/* Noto Sans 48 raster metrics.  A wrapped name uses a TIGHTER internal line
 * advance (LABEL_LINE_STEP) than the font's own line box (67) so the two
 * lines of ONE name read as a single block and never get confused with the
 * gap to the neighbouring row's text. */
#define LABEL_LINE_STEP     58      /* vertical advance between wrapped lines */
#define LABEL_LINE_H        67      /* noto_font_48 full line box height */

/* Scroll physics — real-world.  A release keeps its velocity and coasts
 * under viscous friction (exponential decay, no artificial acceleration, so
 * it can never slow down then speed up).  Friction is deliberately LOW so a
 * fast flick keeps enough kinetic energy to glide roughly half the list.
 * When it has naturally stopped it does one short eased settle onto the
 * nearest row.
 *
 * The EDGE (overscroll) behaviour mirrors LVGL's native scroll elastic:
 * dragging past an end only moves the list at ELASTIC_FOLLOW (¼) finger
 * speed with no hard stop, release momentum that runs past an end dies out
 * quickly (extra EDGE_DAMP) and then the list smoothly animates back onto
 * the boundary — exactly like lv_obj_scroll_by(…, LV_ANIM_ON) after an
 * elastic scroll.  No spring, no oscillation. */
#define THROW_MIN_VEL       40      /* px/s: below this a release goes straight to snap */
#define RELEASE_VMAX        3400.0f /* px/s cap on the measured release velocity */
#define COAST_B             1.0f    /* 1/s viscous friction coefficient (low = long glide) */
#define COAST_STOP          60.0f   /* px/s below which the coast ends & short snap */
#define ELASTIC_FOLLOW      0.25f   /* drag/throw speed factor once past an end */
#define EDGE_DAMP           5.0f    /* 1/s extra friction while travelling past an end */
#define EDGE_STOP           20.0f   /* px/s below which, past an end, animate back */
#define PHYS_MS             16      /* physics timer period (ms) */
#define SNAP_MS             170
#define ELASTIC_MAX         200     /* safety cap on how far the list can over-scroll */
#define VEL_SAMPLES         8
#define VEL_WINDOW_US       150000
#define VEL_STALE_US        250000
#define DRAG_THRESHOLD      10      /* px of movement that turns a press into a drag */

#define LIST_MAX_ROWS       64

/*============================================================================
 * Fixed-point Q16.16 (ESP32-S3 has no FPU)
 *============================================================================*/
#define FP(v)   ((int32_t)((v) * 65536.0f + 0.5f))
#define FMUL(a, b)  ((int32_t)(((int64_t)(a) * (int64_t)(b)) >> 16))

static const int32_t LVL_FP_1    = FP(1.0f);
/* Icon scale: centre 1.07 → gentle falloff (0.24 t²). At t=0.48 (row1)
 * ≈ 1.01 (97 px), at t=0.93 (row2) ≈ 0.86 (83 px). */
static const int32_t ICON_MAG_C  = FP(1.07f);
static const int32_t ICON_MAG_K  = FP(0.24f);
/* Demo-style X drift: whole row (icon + text) drifts RIGHT as it gets
 * farther from centre — centre leftmost, far rows shifted right, max
 * ~30 px (demo: centre x≈61 → ±1 row ≈ +4 → row2 ≈ +24). */
#define X_DRIFT_MAX             30

/*============================================================================
 * Icon table — same set & order as the honeycomb menu (all 47 icons).
 * Each row: [png symbol, menu.<key> (or NULL), English name, action].
 * Actions: -1 = decorative, or one of the shell's pending flags.
 *============================================================================*/
enum { ACT_NONE = 0, ACT_SETTINGS, ACT_PHOTOS, ACT_CALC, ACT_NOISE,
       ACT_ACTIVITY, ACT_JSAPPS, ACT_JSAPP };

typedef struct {
    const lv_image_dsc_t *icon;
    const char           *key;      /* "menu.xxx" lang key, or NULL */
    const char           *name;     /* English default */
    int                   act;
} list_icon_t;

static const list_icon_t s_icons[] = {
    { &settings_96,      "menu.settings",     "Settings",     ACT_SETTINGS },
    { &Activity_96,      "menu.activity",     "Activity",     ACT_ACTIVITY },
    { &alarm_clock_96,   "menu.alarm",        "Alarm",        ACT_NONE },
    { &app_store_96,     "menu.app_store",    "App Store",    ACT_JSAPPS },
    { &Audiobooks_96,    NULL,                "Audiobooks",   ACT_NONE },
    { &breathe_96,       "menu.breathe",      "Breathe",      ACT_NONE },
    { &calculator_96,    "menu.calculator",   "Calculator",   ACT_CALC },
    { &calendar_96,      "menu.calendar",     "Calendar",     ACT_NONE },
    { &camera_96,        "menu.camera",       "Camera",       ACT_NONE },
    { &clock_96,         "menu.clock",        "Clock",        ACT_NONE },
    { &Contacts_96,      NULL,                "Contacts",     ACT_NONE },
    { &Cycle_Tracking_96,NULL,                "Cycle Tracking", ACT_NONE },
    { &Depth_96,         NULL,                "Depth",        ACT_NONE },
    { &ECG_96,           NULL,                "ECG",          ACT_NONE },
    { &Find_Devices_96,  NULL,                "Find Devices", ACT_NONE },
    { &Find_Items_96,    NULL,                "Find Items",   ACT_NONE },
    { &Find_People_96,   NULL,                "Find People",  ACT_NONE },
    { &Heart_Rate_96,    NULL,                "Heart Rate",   ACT_NONE },
    { &home_96,          "menu.home",         "Home",         ACT_NONE },
    { &imessage_96,      "menu.messages",     "Messages",     ACT_NONE },
    { &itunes_96,        "menu.music",        "Music",        ACT_NONE },
    { &mail_96,          "menu.mail",         "Mail",         ACT_NONE },
    { &maps_96,          "menu.maps",         "Maps",         ACT_NONE },
    { &Medicine_96,      NULL,                "Medicine",     ACT_NONE },
    { &Mindfulness_96,   NULL,                "Mindfulness",  ACT_NONE },
    { &Music_96,         "menu.music",        "Music",        ACT_NONE },
    { &News_96,          NULL,                "News",         ACT_NONE },
    { &Noise_96,         "menu.noise",        "Noise",        ACT_NOISE },
    { &Now_Playing_96,   NULL,                "Now Playing",  ACT_NONE },
    { &Padcasts_96,      NULL,                "Podcasts",     ACT_NONE },
    { &phone_96,         "menu.phone",        "Phone",        ACT_NONE },
    { &photos_96,        "menu.photos",       "Photos",       ACT_PHOTOS },
    { &pulse_96,         "menu.pulse",        "Pulse",        ACT_NONE },
    { &reminders_96,     "menu.reminders",    "Reminders",    ACT_NONE },
    { &remote_96,        "menu.remote",       "Remote",       ACT_NONE },
    { &Siren_96,         NULL,                "Siren",        ACT_NONE },
    { &Sleep_96,         NULL,                "Sleep",        ACT_NONE },
    { &stocks_96,        "menu.stocks",       "Stocks",       ACT_NONE },
    { &stopwatch_96,     "menu.stopwatch",    "Stopwatch",    ACT_NONE },
    { &timer_96,         "menu.timer",        "Timer",        ACT_NONE },
    { &Tips_96,          NULL,                "Tips",         ACT_NONE },
    { &Vitals_96,        NULL,                "Vitals",       ACT_NONE },
    { &Voice_Memos_96,   NULL,                "Voice Memos",  ACT_NONE },
    { &wallet_96,        "menu.wallet",       "Wallet",       ACT_NONE },
    { &weather_96,       "menu.weather",      "Weather",      ACT_NONE },
    { &workout_96,       "menu.workout",      "Workout",      ACT_NONE },
    { &world_clock_96,   "menu.world_clock",  "World Clock",  ACT_NONE },
};
#define NATIVE_ICON_COUNT  (sizeof(s_icons) / sizeof(s_icons[0]))

/*============================================================================
 * Per-row state
 *============================================================================*/
typedef struct {
    lv_obj_t              *con;          /* row container (full width, ROW_H) */
    lv_obj_t              *icon;         /* icon image (native or JS src)     */
    lv_obj_t              *label;        /* label image                       */
    const lv_image_dsc_t  *src;          /* icon source in use                */
    int32_t                icon_x0;      /* icon pos X when no X drift        */
    int32_t                label_x0;     /* label pos X when no X drift       */
    int32_t                last_xsh;     /* last applied X drift              */
    int32_t                last_icon_scale;
    int32_t                last_label_scale;
    int                    act;          /* row action (see enum)             */
    int                    js_index;     /* for ACT_JSAPP                     */
} list_row_t;

typedef struct {
    lv_obj_t     *root;
    lv_obj_t     *screen;
    list_row_t    rows[LIST_MAX_ROWS];
    lv_image_dsc_t *label_img[LIST_MAX_ROWS];   /* owned rasterized names    */
    int           n;
    int32_t       S;
    int32_t       smax;
    bool          pressed;
    bool          scroll_active;
    bool          gesture_moved;
    int32_t       scroll_sum;
    int32_t       vel_pos[VEL_SAMPLES];
    int64_t       vel_time[VEL_SAMPLES];
    uint8_t       vel_idx;
    uint8_t       vel_count;
    int           anim_kind;            /* 0 none,1 throw,2 snap,3 click     */
    int           pending_click;        /* row to launch after click anim    */
    float         v;                    /* release velocity (px/s, physics)   */
    lv_timer_t   *phys;                 /* coast + magnet physics timer      */
    int64_t       phys_last;            /* last physics tick time (us)       */
} list_view_state_t;

enum { ANIM_NONE = 0, ANIM_THROW, ANIM_SNAP, ANIM_CLICK };

static list_view_state_t *g = NULL;

/*============================================================================
 * Forward decls
 *============================================================================*/
static void page_event_cb(lv_event_t *e);
static void row_event_cb(lv_event_t *e);
static void list_layout(void);
static int focus_row_from_scroll(int32_t s);
static void start_scroll_physics(void);
static void stop_scroll_physics(void);

/*============================================================================
 * Menu style (persisted via app_config)
 *============================================================================*/
static menu_style_t g_menu_style;
static bool g_menu_style_inited = false;

/*============================================================================
 * UTF-8 decode helper
 *============================================================================*/
static uint32_t utf8_next(const char *s, size_t *i)
{
    uint8_t c = (uint8_t)s[*i];
    if (c < 0x80) { (*i)++; return c; }
    uint32_t cp;
    int extra;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { (*i)++; return 0xFFFD; }
    for (int k = 0; k < extra; k++) {
        uint8_t n = (uint8_t)s[*i + 1 + k];
        if ((n & 0xC0) != 0x80) { cp = 0xFFFD; break; }
        cp = (cp << 6) | (n & 0x3F);
    }
    *i += extra + 1;
    return cp;
}

/*============================================================================
 * White-on-black text rasterizer (Noto Sans 48 → RGB565 image)
 *============================================================================*/
#define TEXT_MAX_LINES      2
static uint16_t s_alpha_rgb565[256];

static void alpha_table_init(void)
{
    static bool inited = false;
    if (inited) return;
    for (int a = 0; a < 256; a++) {
        uint16_t v = 0;
        if (a) {
            v = (uint16_t)((31u * a / 255) << 11);
            v |= (uint16_t)((63u * a / 255) << 5);
            v |= (uint16_t)(31u * a / 255);
        }
        s_alpha_rgb565[a] = v;
    }
    inited = true;
}

static void blit_glyph(uint16_t *pix, int buf_w, int buf_h,
                       int gx, int gy, const uint8_t *bmp,
                       int box_w, int box_h, int bpp)
{
    if (box_w <= 0 || box_h <= 0 || !bmp) return;
    for (int row = 0; row < box_h; row++) {
        int py = gy + row;
        if (py < 0 || py >= buf_h) continue;
        const uint8_t *line = bmp + row * ((box_w * bpp + 7) / 8);
        uint16_t *dst = pix + (size_t)py * buf_w;
        for (int col = 0; col < box_w; col++) {
            int px = gx + col;
            if (px < 0 || px >= buf_w) continue;
            uint8_t a = 0;
            if (bpp == 8)      { a = line[col]; }
            else if (bpp == 4) {
                uint8_t v = line[col >> 1];
                v = (col & 1) ? (v & 0x0F) : (v >> 4);
                a = (uint8_t)(v * 17);
            } else if (bpp == 2) {
                uint8_t v = line[col >> 2];
                v = (uint8_t)((v >> (6 - 2 * (col & 3))) & 0x3);
                a = (uint8_t)(v * 85);
            } else if (bpp == 1) {
                uint8_t v = line[col >> 3];
                v = (uint8_t)((v >> (7 - (col & 7))) & 0x1);
                a = v ? 255 : 0;
            }
            if (a == 0) continue;
            dst[px] = (a == 255) ? 0xFFFF : s_alpha_rgb565[a];
        }
    }
}

static int text_span_w(const char *s, int a, int b, int limit)
{
    const lv_font_t *f = &noto_font_48_bold;
    int w = 0;
    size_t i = (size_t)a;
    uint32_t prev = 0;
    while ((int)i < b && s[i]) {
        uint32_t letter = utf8_next(s, &i);
        if (w >= limit) break;
        lv_font_glyph_dsc_t gd;
        if (lv_font_get_glyph_dsc(f, &gd, letter, prev)) w += gd.adv_w;
        prev = letter;
    }
    return w;
}

/* Greedy word-wrap into at most TEXT_MAX_LINES byte ranges.  Returns the
 * number of lines; ls[]/le[] hold each line's start/end byte offsets.
 *
 * Splitting happens ONLY between words (space-separated).  A single word
 * (e.g. a long Latin or CJK name with no spaces) is NEVER split across
 * lines — it is left as one line and simply rendered at its full width. */
static int wrap_text_lines(const char *text, int *ls, int *le)
{
    const int len = (int)strlen(text);

    int space_w = 0;
    {
        lv_font_glyph_dsc_t gd;
        if (lv_font_get_glyph_dsc(&noto_font_48_bold, &gd, ' ', 0)) space_w = gd.adv_w;
    }

    int st[32], en[32];
    int nw = 0, i = 0;
    while (i < len && nw < 32) {
        while (i < len && text[i] == ' ') i++;
        if (i >= len) break;
        int a = i;
        while (i < len && text[i] != ' ') i++;
        st[nw] = a; en[nw] = i; nw++;
    }
    if (nw == 0) return 0;

    /* Pack whole words greedily; each new line starts on a word boundary. */
    int n = 0;
    int openw = 0;                    /* width of the open line (excl. trailing space) */
    int openStart = st[0];            /* byte where the open line's text starts */
    int openEnd = en[0];              /* byte end of the last word on the open line */
    int w;

    for (w = 0; w < nw && n < TEXT_MAX_LINES; w++) {
        int ww = text_span_w(text, st[w], en[w], 100000);
        int need = openw + (openw ? space_w : 0) + ww;
        if (w == 0) {                 /* first word always opens line 1 */
            openw = ww;
            openEnd = en[0];
            continue;
        }
        if (need <= TEXT_MAX_W) {
            openw = need;
            openEnd = en[w];
        } else {
            ls[n] = openStart; le[n] = openEnd; n++;
            if (n >= TEXT_MAX_LINES) break;
            openStart = st[w];
            openw = ww;
            openEnd = en[w];
        }
    }
    if (n < TEXT_MAX_LINES) {
        ls[n] = openStart; le[n] = openEnd; n++;
    }
    return n;
}

/* Raster the app name at FULL Noto 48 into a white-on-black RGB565 image,
 * wrapped onto up to two lines, left-aligned (aligned with the icon). */
static lv_image_dsc_t *label_raster(const char *text)
{
    if (!text || !text[0]) return NULL;
    const lv_font_t *f = &noto_font_48_bold;

    int ls[TEXT_MAX_LINES], le[TEXT_MAX_LINES];
    int nl = wrap_text_lines(text, ls, le);
    if (nl < 1) return NULL;

    /* Image width = the widest line at its FULL natural width.  Wrapped
     * lines are ≤ TEXT_MAX_W, but a single over-long word (never split)
     * may be wider — allow it up to LABEL_MAX_W, the most that fits on the
     * panel next to the icon (even with the full X drift). */
    int W = 0;
    for (int k = 0; k < nl; k++) {
        int w = text_span_w(text, ls[k], le[k], 100000);
        if (w > W) W = w;
    }
    if (W < 8) W = 8;
    if (W > LABEL_MAX_W) W = LABEL_MAX_W;

    const int line_h = f->line_height;                  /* LABEL_LINE_H 67 */
    /* Wrapped lines use the tighter LABEL_LINE_STEP advance so the two lines
     * of one name sit close together; the LAST line still keeps its full
     * line box so its glyphs are never clipped off at the image bottom. */
    const int step = (nl > 1) ? LABEL_LINE_STEP : line_h;
    const int H = (nl - 1) * step + line_h;
    size_t bytes = (size_t)W * H * 2;

    uint16_t *pix = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!pix) return NULL;
    memset(pix, 0, bytes);
    alpha_table_init();

    const int baseline = f->line_height - f->base_line; /* 52 inside a line */
    for (int k = 0; k < nl; k++) {
        int pen = 0;
        size_t pos = (size_t)ls[k];
        uint32_t prev = 0;
        while (pos < (size_t)le[k] && text[pos]) {
            uint32_t letter = utf8_next(text, &pos);
            if (pos > (size_t)le[k]) break;
            lv_font_glyph_dsc_t gd;
            if (!lv_font_get_glyph_dsc(f, &gd, letter, prev)) { prev = letter; continue; }
            prev = letter;
            if (pen >= W) break;
            if (gd.box_w > 0 && gd.box_h > 0 &&
                (gd.format == LV_FONT_GLYPH_FORMAT_A1 ||
                 gd.format == LV_FONT_GLYPH_FORMAT_A2 ||
                 gd.format == LV_FONT_GLYPH_FORMAT_A4 ||
                 gd.format == LV_FONT_GLYPH_FORMAT_A8)) {
                gd.req_raw_bitmap = 1;
                const lv_font_t *rf = gd.resolved_font ? gd.resolved_font : f;
                const uint8_t *bmp = rf->get_glyph_bitmap(&gd, NULL);
                if (bmp) {
                    int gy = k * step + baseline - gd.box_h - gd.ofs_y;
                    int gx = pen + gd.ofs_x;
                    blit_glyph(pix, W, H, gx, gy, bmp, gd.box_w, gd.box_h,
                               (int)gd.format);
                }
            }
            pen += gd.adv_w;
        }
    }

    lv_image_dsc_t *dsc = heap_caps_malloc(sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM);
    if (!dsc) { heap_caps_free(pix); return NULL; }
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.w = W;
    dsc->header.h = H;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->data = (const void *)pix;
    dsc->data_size = (uint32_t)bytes;
    return dsc;
}

static void list_label_free(lv_image_dsc_t *dsc)
{
    if (!dsc) return;
    if (dsc->data) heap_caps_free((void *)dsc->data);
    heap_caps_free(dsc);
}

/*============================================================================
 * Localized / default name
 *============================================================================*/
static const char *row_display_name(const list_icon_t *ic)
{
    if (!ic->key) return ic->name;
    const char *s = app_lang_get(ic->key);
    /* app_lang_get returns the raw key when the key is missing — fall back. */
    if (s && s[0] && strncmp(s, "menu.", 5) != 0) return s;
    return ic->name;
}

/*============================================================================
 * Row / UI construction
 *============================================================================*/
static void row_build(int i, const lv_image_dsc_t *icon, const char *name,
                      int act, int js_index)
{
    list_row_t *r = &g->rows[i];

    lv_obj_t *con = lv_obj_create(g->root);
    lv_obj_remove_flag(con, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(con, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(con, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(con, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  /* never clip 2-line label */
    lv_obj_set_style_bg_opa(con, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(con, 0, 0);
    lv_obj_set_style_radius(con, 0, 0);
    lv_obj_set_style_pad_all(con, 0, 0);
    lv_obj_set_size(con, LCD_H_RES, ROW_H);
    lv_obj_add_event_cb(con, row_event_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
    r->con = con;
    r->act = act;
    r->js_index = js_index;

    /* ── Icon ─────────────────────────────────────────────────────── */
    if (icon) {
        int w = icon->header.w, h = icon->header.h;
        if (w > 0 && h > 0) {
            lv_obj_t *img = lv_image_create(con);
            lv_image_set_src(img, icon);
            lv_image_set_pivot(img, w >> 1, h >> 1);
            lv_obj_set_pos(img, ICON_X - (w >> 1), (ROW_H - h) >> 1);
            r->icon = img;
            r->src = icon;
            r->icon_x0 = ICON_X - (w >> 1);
        }
    }

    /* ── App name: full-size Noto 48, wraps to ≤2 lines, never scaled.
     * The whole 1/2-line block is vertically centred on the icon. */
    if (name && name[0]) {
        lv_image_dsc_t *ld = label_raster(name);
        if (ld) {
            int lh = ld->header.h;
            lv_obj_t *img = lv_image_create(con);
            lv_image_set_src(img, ld);
            lv_obj_set_pos(img, TEXT_X, (ROW_H >> 1) - (lh >> 1));
            r->label = img;
            r->label_x0 = TEXT_X;
            g->label_img[i] = ld;
        }
    }
}

/*============================================================================
 * Fisheye layout — constant circle-edge gap, solved every frame.
 *
 * The scroll variable S (px) picks the anchor: the nearest row
 * a = round(S / ROW_PITCH) is placed a touch off centre (so the list
 * translates smoothly while dragging), and every other row is solved so the
 * SHORTEST distance between two neighbouring icon circumferences is always
 * GAP_EDGE px — even as the icons scale up/down.  Because the solver runs
 * every frame (drag, coast, magnet and rest alike), the gap never changes.
 * A few Gauss-Seidel sweeps outward from the anchor converge it (each row's
 * radius depends on its own distance from the screen centre).
 *============================================================================*/

/* Display radius (px) of the row icon when its centre is at yc. */
static int32_t row_radius_px(int32_t yc)
{
    int32_t ad = yc - LIST_CY;
    if (ad < 0) ad = -ad;
    int32_t t16 = (int32_t)(((int64_t)ad << 16) / T_NORM);
    if (t16 > LVL_FP_1) t16 = LVL_FP_1;
    int32_t t2 = FMUL(t16, t16);
    int32_t icmag = ICON_MAG_C - FMUL(ICON_MAG_K, t2);
    int32_t disp = (int32_t)(((int64_t)96 * icmag + 32768) >> 16);
    return disp / 2;
}

/* Place one row with its centre at yc (screen Y). */
static void row_place(int i, int32_t yc)
{
    list_row_t *r = &g->rows[i];
    if (!r->con) return;

    int32_t dy = yc - LIST_CY;
    int32_t ad = (dy < 0) ? -dy : dy;
    if (ad > VIS_RANGE) {
        if (!lv_obj_has_flag(r->con, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(r->con, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (lv_obj_has_flag(r->con, LV_OBJ_FLAG_HIDDEN))
        lv_obj_clear_flag(r->con, LV_OBJ_FLAG_HIDDEN);

    int32_t t16 = (int32_t)(((int64_t)ad << 16) / T_NORM);
    if (t16 > LVL_FP_1) t16 = LVL_FP_1;
    int32_t t2 = FMUL(t16, t16);
    int32_t icmag = ICON_MAG_C - FMUL(ICON_MAG_K, t2);

    int32_t cont_y = yc - (ROW_H >> 1);
    if (lv_obj_get_y(r->con) != cont_y) lv_obj_set_y(r->con, cont_y);

    /* Demo-style rightward X drift: xsh = X_DRIFT_MAX·t³. */
    int32_t t3 = FMUL(t2, t16);
    int32_t xsh = (int32_t)(((int64_t)X_DRIFT_MAX * t3) >> 16);
    if (xsh != r->last_xsh) {
        r->last_xsh = xsh;
        if (r->icon) lv_obj_set_x(r->icon, r->icon_x0 + xsh);
        if (r->label) lv_obj_set_x(r->label, r->label_x0 + xsh);
    }

    if (r->icon) {
        int32_t srcw = r->src ? r->src->header.w : ICON_NATIVE;
        if (srcw <= 0) srcw = ICON_NATIVE;
        int32_t disp = (int32_t)(((int64_t)96 * icmag + 32768) >> 16);
        int32_t s = (int32_t)(((int64_t)disp * 256) / srcw);
        if (s < 16) s = 16;
        if (s != r->last_icon_scale) {
            lv_image_set_scale(r->icon, s);
            r->last_icon_scale = s;
        }
    }
}

/* Continuous layout — anchor row a ≈ S/ROW_PITCH, rows solved to keep a
 * constant circumference gap.  Works for drag, coast, magnet and rest. */
static void list_layout(void)
{
    if (!g) return;

    const int n = g->n;
    if (n <= 0) return;

    int a = (g->S + ROW_PITCH / 2) / ROW_PITCH;
    if (a < 0) a = 0;
    if (a >= n) a = n - 1;
    int32_t t = g->S - a * ROW_PITCH;      /* [-P/2, +P/2] offset of row a */

    int32_t y[LIST_MAX_ROWS];
    y[a] = LIST_CY - t;
    for (int i = a - 1; i >= 0; i--) y[i] = y[i + 1] - ROW_PITCH;
    for (int i = a + 1; i < n; i++) y[i] = y[i - 1] + ROW_PITCH;

    /* Outward Gauss-Seidel sweeps: make each neighbour gap GAP_EDGE + r_i
     * + r_{i+1} (r depends on distance from centre, so iterate a few times). */
    for (int pass = 0; pass < 3; pass++) {
        for (int i = a + 1; i < n; i++)
            y[i] = y[i - 1] + GAP_EDGE + row_radius_px(y[i - 1]) + row_radius_px(y[i]);
        for (int i = a - 1; i >= 0; i--)
            y[i] = y[i + 1] - (GAP_EDGE + row_radius_px(y[i]) + row_radius_px(y[i + 1]));
    }

    for (int i = 0; i < n; i++) row_place(i, y[i]);
}

/*============================================================================
 * Destroy
 *============================================================================*/
void list_view_menu_destroy(void);

static void row_launch(int i)
{
    if (!g || i < 0 || i >= g->n) return;
    const list_row_t *r = &g->rows[i];
    switch (r->act) {
    case ACT_SETTINGS:   g_pending_settings   = true; break;
    case ACT_PHOTOS:     g_pending_photos     = true; break;
    case ACT_CALC:       g_pending_calculator = true; break;
    case ACT_NOISE:      g_pending_noise      = true; break;
    case ACT_ACTIVITY:   g_pending_activity   = true; break;
    case ACT_JSAPPS:     g_pending_jsapps     = true; break;
    case ACT_JSAPP:      g_pending_jsapp      = r->js_index; break;
    default:             return;                 /* decorative → no launch */
    }
    ESP_LOGI(TAG, "Launch row %d", i);
    list_view_menu_destroy();
}

/*============================================================================
 * Scroll animations (var == g)
 *============================================================================*/
static void lv_anim_exec(void *var, int32_t value)
{
    (void)var;
    if (!g) return;
    g->S = value;
    list_layout();
    if (g->root) lv_obj_invalidate(g->root);
}

static void lv_anim_done(lv_anim_t *a)
{
    (void)a;
    if (!g) return;
    int kind = g->anim_kind;
    g->anim_kind = ANIM_NONE;
    if (kind == ANIM_CLICK && g->pending_click >= 0) {
        int i = g->pending_click;
        g->pending_click = -1;
        row_launch(i);          /* destroys g — do not touch g after */
    }
}

static void lv_anim_to(int32_t target)
{
    if (!g) return;
    if (g->anim_kind != ANIM_NONE) lv_anim_del(g, lv_anim_exec);
    g->anim_kind = ANIM_CLICK;

    int32_t from = g->S;
    if (target < 0) target = 0;
    if (target > g->smax) target = g->smax;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g);
    lv_anim_set_exec_cb(&a, lv_anim_exec);
    lv_anim_set_values(&a, from, target);
    lv_anim_set_completed_cb(&a, lv_anim_done);
    lv_anim_set_time(&a, SNAP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/*============================================================================
 * Scroll state helpers
 *============================================================================*/
static void vel_reset(void)
{
    if (!g) return;
    g->vel_count = 0;
    g->vel_idx = 0;
}

static void vel_sample(void)
{
    if (!g) return;
    g->vel_pos[g->vel_idx] = g->S;
    g->vel_time[g->vel_idx] = esp_timer_get_time();
    g->vel_idx = (uint8_t)((g->vel_idx + 1) % VEL_SAMPLES);
    if (g->vel_count < VEL_SAMPLES) g->vel_count++;
}

static int32_t vel_compute(void)
{
    if (!g || g->vel_count == 0) return 0;
    int ni = (g->vel_idx - 1 + VEL_SAMPLES) % VEL_SAMPLES;
    int64_t t1 = g->vel_time[ni];
    int32_t p1 = g->vel_pos[ni];
    if ((esp_timer_get_time() - t1) > VEL_STALE_US) return 0;

    /* Prefer a FRESH velocity from the last two samples — a short fast flick
     * must not be diluted by the older (slower) part of the drag window. */
    if (g->vel_count >= 2) {
        int prev = (ni - 1 + VEL_SAMPLES) % VEL_SAMPLES;
        int64_t dt2 = t1 - g->vel_time[prev];
        if (dt2 >= 8000 && dt2 <= VEL_WINDOW_US) {
            return (int32_t)(((int64_t)(p1 - g->vel_pos[prev]) * 1000000) / dt2);
        }
    }

    /* Fallback: average over the recent window. */
    int64_t t0 = t1;
    int32_t p0 = p1;
    for (int k = 0; k < g->vel_count; k++) {
        int id = (ni - k + VEL_SAMPLES) % VEL_SAMPLES;
        if ((t1 - g->vel_time[id]) > VEL_WINDOW_US) break;
        p0 = g->vel_pos[id];
        t0 = g->vel_time[id];
    }
    int64_t dt = t1 - t0;
    if (dt < 20000) dt = 20000;
    return (int32_t)(((int64_t)(p1 - p0) * 1000000) / dt);
}

static int focus_row_from_scroll(int32_t s)
{
    if (!g || g->n <= 0) return 0;
    int f = (s + ROW_PITCH / 2) / ROW_PITCH;
    if (f < 0) f = 0;
    if (f > g->n - 1) f = g->n - 1;
    return f;
}

/*============================================================================
 * Scroll physics — release → viscous coast → eased row snap
 *
 * After a release a tiny physics timer runs: while the speed is high the
 * list only coasts under viscous friction (exponential decay — it can never
 * slow down then speed back up).  When it has naturally stopped it does one
 * short eased settle onto the nearest row.  Once the list is pushed past an
 * end, the elastic behaves like LVGL's native scroll: outward momentum dies
 * out quickly and the list smoothly animates back onto the boundary.
 *============================================================================*/

static void phys_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!g) return;

    /* Real elapsed time (not the fixed period) so physics is frame-rate
     * independent. */
    int64_t now = esp_timer_get_time();
    float dt = (float)(now - g->phys_last) / 1000000.0f;
    if (dt <= 0.0005f) dt = 0.0005f;
    if (dt > 0.050f) dt = 0.050f;
    g->phys_last = now;

    float v = g->v;
    float sp = (v < 0.0f) ? -v : v;
    bool overscroll = (g->S < 0) || (g->S > g->smax);

    if (overscroll) {
        /* LVGL-style elastic: the list is past an end.  LVGL does NOT use a
         * spring — dragging past the edge only moves the list at ¼ speed and
         * momentum that runs past an end is damped hard so it dies out just
         * past the boundary; then the list animates back onto the edge with a
         * short eased motion (like lv_obj_scroll_by(…, LV_ANIM_ON)). */
        int32_t bnd = (g->S < 0) ? 0 : g->smax;
        bool moving_out = (g->S < 0) ? (v < 0) : (v > 0);

        if (moving_out && sp > EDGE_STOP) {
            g->S += (int32_t)(v * dt * ELASTIC_FOLLOW);
            v *= expf(-(COAST_B + EDGE_DAMP) * dt);   /* dies out quickly */
        } else {
            v = 0.0f;                                  /* nothing pushing outward */
        }

        if (fabsf(v) < EDGE_STOP) {
            g->v = 0.0f;
            stop_scroll_physics();
            if (g->S != bnd) {
                g->pending_click = -1;
                lv_anim_to(bnd);                       /* smooth LV_ANIM return */
            } else {
                list_layout();
                if (g->root) lv_obj_invalidate(g->root);
            }
            return;
        }

        /* Safety cap so a wild throw can't hide every row in the void. */
        if (g->S < -ELASTIC_MAX)            g->S = -ELASTIC_MAX;
        else if (g->S > g->smax + ELASTIC_MAX) g->S = g->smax + ELASTIC_MAX;

        g->v = v;
        list_layout();
        if (g->root) lv_obj_invalidate(g->root);
        return;
    }

    if (sp >= COAST_STOP) {
        /* Real coast: position follows velocity; velocity decays by viscous
         * friction.  No spring while moving → never speeds back up.  If it
         * crosses an end the elastic branch above takes over next tick. */
        g->S += (int32_t)(v * dt);
        v *= expf(-COAST_B * dt);
    } else {
        /* Natural rest inside the list → one short eased settle onto the
         * nearest row. */
        int f = focus_row_from_scroll(g->S);
        int32_t target = f * ROW_PITCH;
        int32_t err = target - g->S;
        g->v = 0.0f;
        stop_scroll_physics();
        if (err < -1 || err > 1) {
            g->pending_click = -1;
            lv_anim_to(target);
        } else {
            g->S = target;
        }
        list_layout();
        if (g->root) lv_obj_invalidate(g->root);
        return;
    }

    g->v = v;
    list_layout();
    if (g->root) lv_obj_invalidate(g->root);
}

static void start_scroll_physics(void)
{
    if (!g || g->phys) return;
    g->phys_last = esp_timer_get_time();
    g->phys = lv_timer_create(phys_tick, PHYS_MS, g);
    if (g->phys) lv_timer_ready(g->phys);
}

static void stop_scroll_physics(void)
{
    if (!g) return;
    if (g->phys) {
        lv_timer_t *t = g->phys;
        g->phys = NULL;
        lv_timer_delete(t);
    }
    g->v = 0.0f;
}

/*============================================================================
 * Event callbacks
 *============================================================================*/
static void page_event_cb(lv_event_t *e)
{
    if (!g) return;
    lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_PRESSED:
        stop_scroll_physics();
        g->anim_kind = ANIM_NONE;
        g->pending_click = -1;
        lv_anim_del(g, lv_anim_exec);
        g->gesture_moved = false;
        g->scroll_active = false;
        g->scroll_sum = 0;
        g->pressed = true;
        vel_reset();
        break;

    case LV_EVENT_PRESSING: {
        lv_indev_t *indev = lv_indev_active();
        if (!indev) break;
        lv_point_t p;
        lv_indev_get_vect(indev, &p);
        g->scroll_sum += p.y;
        int32_t abs_sum = (g->scroll_sum < 0) ? -g->scroll_sum : g->scroll_sum;
        if (abs_sum > DRAG_THRESHOLD || g->scroll_active) {
            g->scroll_active = true;
            g->gesture_moved = true;
        }
        if (g->scroll_active) {
            /* Elastic drag — LVGL behaviour: inside [0,smax] the list follows
             * the finger 1:1; past an edge it only moves at ELASTIC_FOLLOW
             * (¼) finger speed (lv_indev_scroll.c: ELASTIC_SLOWNESS_FACTOR 4),
             * no hard stop.  On release the physics animates it back. */
            float ns = (float)(g->S - p.y);
            if (ns < 0.0f)            g->S = (int32_t)(ns * ELASTIC_FOLLOW);
            else if (ns > g->smax)    g->S = g->smax + (int32_t)((ns - g->smax) * ELASTIC_FOLLOW);
            else                      g->S = (int32_t)ns;
            vel_sample();
            list_layout();
            if (g->root) lv_obj_invalidate(g->root);
        }
        break;
    }

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        g->pressed = false;
        if (g->scroll_active) {
            float rv = (float)vel_compute();
            float ra = (rv < 0.0f) ? -rv : rv;
            if (ra < THROW_MIN_VEL) rv = 0.0f;
            else if (ra > RELEASE_VMAX) rv = (rv > 0.0f) ? RELEASE_VMAX : -RELEASE_VMAX;
            g->v = rv;
            start_scroll_physics();
        }
        g->scroll_active = false;
        g->scroll_sum = 0;
        break;

    case LV_EVENT_CLICKED:
        g->pressed = false;
        g->scroll_active = false;
        g->scroll_sum = 0;
        break;

    default:
        break;
    }
}

static void row_event_cb(lv_event_t *e)
{
    if (!g) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_SHORT_CLICKED) return;
    if (g->gesture_moved || g->scroll_active) return;

    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= g->n) return;
    const list_row_t *r = &g->rows[i];
    if (r->act == ACT_NONE) return;   /* decorative icon → no launch */

    ESP_LOGI(TAG, "Row %d clicked", i);
    int f = focus_row_from_scroll(g->S);
    if (f == i) {
        row_launch(i);
    } else {
        g->pending_click = i;
        lv_anim_to(i * ROW_PITCH);
    }
}

/*============================================================================
 * Fade-in
 *============================================================================*/
static void fade_exec(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/*============================================================================
 * Public API
 *============================================================================*/
void app_menu_set_style(menu_style_t style)
{
    g_menu_style = style;
    g_menu_style_inited = true;
    g_config.menu_style = (uint8_t)style;
    app_config_save();
    ESP_LOGI(TAG, "Menu style: %s", style == MENU_STYLE_LIST ? "list" : "honeycomb");
}

menu_style_t app_menu_get_style(void)
{
    if (!g_menu_style_inited) {
        g_menu_style = (menu_style_t)g_config.menu_style;
        g_menu_style_inited = true;
    }
    return g_menu_style;
}

void list_view_menu_create(void)
{
    ESP_LOGI(TAG, "Creating list view menu...");

    g = heap_caps_calloc(1, sizeof(list_view_state_t), MALLOC_CAP_SPIRAM);
    if (!g) { ESP_LOGE(TAG, "OOM"); return; }

    js_app_registry_scan();

    /* Rows: all 47 native icons, then SD JS apps appended. */
    int n = (int)NATIVE_ICON_COUNT + js_app_registry_count();
    if (n > LIST_MAX_ROWS) n = LIST_MAX_ROWS;
    g->n = n;
    g->smax = (n > 0) ? (n - 1) * ROW_PITCH : 0;
    g->pending_click = -1;

    g->screen = lv_screen_active();
    lv_obj_t *root = lv_obj_create(g->screen);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(root, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    g->root = root;

    int i = 0;
    for (; i < (int)NATIVE_ICON_COUNT && i < n; i++) {
        const list_icon_t *ic = &s_icons[i];
        row_build(i, ic->icon, row_display_name(ic), ic->act, -1);
    }
    for (int j = 0; i < n && j < js_app_registry_count(); j++) {
        const js_app_entry_t *je = js_app_registry_get(j);
        if (!je) break;
        row_build(i, je->icon, je->name, ACT_JSAPP, j);
        i++;
    }
    g->n = i;
    g->smax = (g->n > 0) ? (g->n - 1) * ROW_PITCH : 0;

    /* Open on the SECOND row centred (matches the reference demo); row 0
     * then sits just above centre.  With a single row we start on it. */
    int entry = (g->n > 1) ? 1 : 0;
    g->S = entry * ROW_PITCH;
    if (g->S > g->smax) g->S = g->smax;

    lv_obj_add_event_cb(root, page_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, page_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, page_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(root, page_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(root, page_event_cb, LV_EVENT_CLICKED, NULL);

    /* Entry layout: the continuous constant-gap solver (S is already an
     * exact row multiple, so the anchor row sits dead centre). */
    list_layout();

    /* Fade the whole menu in. */
    lv_obj_set_style_opa(root, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root);
    lv_anim_set_exec_cb(&a, fade_exec);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "List view menu ready (%d rows, scroll=%" LV_PRId32 ")", g->n, g->S);
}

void list_view_menu_destroy(void)
{
    if (!g) return;
    ESP_LOGI(TAG, "Destroying list view menu...");

    stop_scroll_physics();
    lv_anim_del(g, lv_anim_exec);
    if (g->root) lv_anim_del(g->root, fade_exec);

    if (g->root) {
        lv_obj_del(g->root);
        g->root = NULL;
    }
    for (int i = 0; i < g->n; i++) {
        list_label_free(g->label_img[i]);
        g->label_img[i] = NULL;
    }

    heap_caps_free(g);
    g = NULL;
    ESP_LOGI(TAG, "List view menu destroyed");
}

bool list_view_menu_is_active(void)
{
    return (g != NULL);
}
