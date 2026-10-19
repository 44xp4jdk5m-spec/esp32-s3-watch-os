/**
 * @file control_center.c
 * @brief Apple-Watch-style control center (see control_center.h).
 *
 * Layout mirrors tmp/control_center/ui_control_center.c:
 *   - 410x502 screen, black, NO radius clip (the physical glass is radius 100).
 *   - A 2-column grid of 177x120 pill buttons (side margin 24, col gap 8,
 *     row gap 10).  Inner edges stay fixed while buttons scale, so the
 *     8px center gap is constant.
 *   - A floating status pill at the top (only when at least one status
 *     icon is active), width grows with the number of icons.
 *   - Scroll-scale effect (like Settings) + a geometric corner-fit
 *     pre-calc that shrinks buttons near the rounded screen corners.
 *
 * The blurred backdrop is a ONE-TIME heavy box blur of a screenshot of
 * whatever screen was active when the center opened (not a live blur).
 */

#include "control_center.h"

#include "lvgl.h"
#include "draw/snapshot/lv_snapshot.h"
#include "draw/lv_draw_buf.h"
#include "screen_metrics.h"
#include "sw_theme.h"
#include "sw_icons.h"
#include "noto_font_48_bold.h"
#include "mi_font_60.h"
#include "app_events.h"
#include "app_config.h"
#include "app_power.h"
#include "app_wifi.h"
#include "app_ble_ancs.h"
#include "hw_vibrator.h"
#include "settings_ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CC";

/* ── Geometry (demo values) ────────────────────────────────────────── */
#define CC_W            EOS_DISPLAY_W   /* 410 */
#define CC_H            EOS_DISPLAY_H   /* 502 */
#define CC_CORNER_R     100             /* physical rounded-corner radius  */
#define CC_MARGIN       24              /* side gap (demo)                */
#define CC_BTN_W        177
#define CC_BTN_H        120
#define CC_ROW_GAP      10
#define CC_ROW_PITCH    (CC_BTN_H + CC_ROW_GAP)   /* 130 */
#define CC_LEFT_X       24
#define CC_RIGHT_X      209
#define CC_LEFT_INNER   (CC_LEFT_X + CC_BTN_W)    /* 201: left btn right edge  */
#define CC_RIGHT_INNER  CC_RIGHT_X                /* 209: right btn left edge  */
#define CC_STATUS_Y     30
#define CC_STATUS_H     40
#define CC_ICON_SLOT    25
#define CC_TOP_WITH_STATUS  104
#define CC_TOP_NO_STATUS    52
/* Extra scrollable space below the last button so it can reach screen
 * centre (instead of resting at the bottom edge where the scroll scale
 * would shrink it) — like the hidden tail padding Settings uses. */
#define CC_BOTTOM_PAD   190

#define CC_MIN_SCALE    110   /* ~43% — never scales to 0 */
#define CC_BOUNCE_DIST  90    /* elastic pull-down when GPIO10 at top  */

/* ── Palette ───────────────────────────────────────────────────────── */
#define CC_ACTIVE_BG    0x4892F1
#define CC_SILENT_BG    0xFF3B30
#define CC_LOW_POWER_BG 0xFF9F0A   /* orange-yellow: Low Power Mode ON */
#define CC_INACTIVE_BG  0x101010
#define CC_ICON_ON      0xF6FBFE
#define CC_ICON_OFF     0x939594

#define CC_BLUR_R       20    /* frosted-base blur radius               */
#define CC_BLOOM_THRESH 24    /* luma below this = treated as black      */
#define CC_BLOOM_AMP    200   /* non-black pixels normalised to this peak */
#define CC_BLOOM_R      26    /* wide glow-spread radius                 */
#define CC_BLOOM_ADD    210   /* glow added back onto the base           */

/* ── Button types ──────────────────────────────────────────────────── */
typedef enum {
    CC_BTN_WIFI = 0,
    CC_BTN_BT,
    CC_BTN_SILENT,
    CC_BTN_FIND,
    CC_BTN_DND,
    CC_BTN_AIRPLANE,
    CC_BTN_FLASHLIGHT,
    CC_BTN_BRIGHTNESS,
    CC_BTN_VOLUME,
    CC_BTN_SETTINGS,
    CC_BTN_REBOOT,
    CC_BTN_AOD,
    CC_BTN_LOW_POWER,
    CC_BTN_BATTERY,
    CC_BTN_COUNT
} cc_btn_t;

static const char *const s_icons[CC_BTN_COUNT] = {
    [CC_BTN_WIFI]       = SW_ICON_WIFI,
    [CC_BTN_BT]         = SW_ICON_BLUETOOTH,
    [CC_BTN_SILENT]     = SW_ICON_VOLUME_OFF,
    [CC_BTN_FIND]       = SW_ICON_WIFI_FIND,
    [CC_BTN_DND]        = MI_MOON,              /* do-not-disturb (moon)   */
    [CC_BTN_AIRPLANE]   = MI_AIRPLANE_MODE,     /* airplanemode_active     */
    [CC_BTN_FLASHLIGHT] = MI_FLASHLIGHT,        /* flashlight_on           */
    [CC_BTN_BRIGHTNESS] = SW_ICON_SUN,          /* brightness (light_mode) */
    [CC_BTN_VOLUME]     = SW_ICON_VOLUME_UP,
    [CC_BTN_SETTINGS]   = SW_ICON_SETTINGS,     /* gear                     */
    [CC_BTN_REBOOT]     = SW_ICON_POWER,        /* power / reboot           */
    [CC_BTN_AOD]        = NULL,                 /* text button              */
    [CC_BTN_LOW_POWER]  = MI_BATTERY_CHARGING,  /* battery / power-saver    */
    [CC_BTN_BATTERY]    = NULL,                 /* label button             */
};

/* Grid order — one entry per button, laid out 2 per row (left/right).
 * Edit this table to rearrange. */
static const cc_btn_t s_order[CC_BTN_COUNT] = {
    CC_BTN_AIRPLANE,    /* row 0 L */
    CC_BTN_WIFI,        /* row 0 R */
    CC_BTN_BT,          /* row 1 L */
    CC_BTN_BATTERY,     /* row 1 R */
    CC_BTN_SILENT,      /* row 2 L */
    CC_BTN_FLASHLIGHT,  /* row 2 R */
    CC_BTN_DND,         /* row 3 L */
    CC_BTN_FIND,        /* row 3 R */
    CC_BTN_BRIGHTNESS,  /* row 4 L */
    CC_BTN_VOLUME,      /* row 4 R */
    CC_BTN_AOD,         /* row 5 L */
    CC_BTN_LOW_POWER,   /* row 5 R */
    CC_BTN_SETTINGS,    /* row 6 L */
    CC_BTN_REBOOT,      /* row 6 R */
};

typedef struct {
    cc_btn_t  type;
    lv_obj_t *obj;    /* the button */
    lv_obj_t *icon;   /* icon label (or NULL for the battery) */
    lv_obj_t *label;  /* battery text label */
    bool      full;   /* full-width battery row */
    bool      right;  /* right column (pivot at left edge) */
} cc_item_t;

/* ── Static state ──────────────────────────────────────────────────── */
static bool           s_active   = false;
static lv_obj_t      *s_scr      = NULL;
static lv_obj_t      *s_grid     = NULL;
static lv_obj_t      *s_status   = NULL;
static lv_obj_t      *s_prev_scr = NULL;
static lv_timer_t    *s_scroll_timer = NULL;
static lv_timer_t    *s_state_timer  = NULL;
static uint8_t       *s_blur_data = NULL;  /* blurred RGB565 (PSRAM) */
static lv_img_dsc_t   s_blur_dsc;
static cc_item_t      s_items[CC_BTN_COUNT];
static uint8_t        s_status_set = 0;    /* bitmask of active status icons */

/* Status icon bits */
enum { ST_WIFI = 1, ST_PHONE = 2, ST_CHARGING = 4, ST_SILENT = 8 };

static void cc_refresh_states(void);

/* ── Blur (separable box blur over RGB888) ─────────────────────────── */

typedef struct { uint8_t r, g, b; } cc_rgb_t;

static void cc_rgb888_to_565(const cc_rgb_t *src, uint16_t *dst, int n)
{
    for (int i = 0; i < n; i++) {
        dst[i] = (uint16_t)(((uint16_t)(src[i].r >> 3) << 11) |
                            ((uint16_t)(src[i].g >> 2) << 5)  |
                            (src[i].b >> 3));
    }
}

/* One separable box-blur pass with edge-clamped (replicate) window.
 * Uses a proper sliding window: the window [lo..hi] is initialised to
 * [0..r] and then slides, so the running sum is always divided by the
 * exact number of pixels it contains. */
static void cc_box_pass(cc_rgb_t *dst, const cc_rgb_t *src,
                        int w, int h, int r, bool horizontal)
{
    if (horizontal) {
        for (int y = 0; y < h; y++) {
            const cc_rgb_t *row = src + (size_t)y * w;
            cc_rgb_t       *drow = dst + (size_t)y * w;
            int rs = 0, gs = 0, bs = 0;
            int lo = 0;
            int hi = (r < w) ? r : (w - 1);
            for (int i = 0; i <= hi; i++) { rs += row[i].r; gs += row[i].g; bs += row[i].b; }
            int win = hi + 1;
            for (int x = 0; x < w; x++) {
                drow[x].r = (uint8_t)(rs / win);
                drow[x].g = (uint8_t)(gs / win);
                drow[x].b = (uint8_t)(bs / win);

                int nlo = x + 1 - r; if (nlo < 0) nlo = 0;
                int nhi = x + 1 + r; if (nhi >= w) nhi = w - 1;
                if (nhi > hi) { rs += row[nhi].r; gs += row[nhi].g; bs += row[nhi].b; win++; }
                if (nlo > lo) { rs -= row[lo].r; gs -= row[lo].g; bs -= row[lo].b; win--; }
                lo = nlo; hi = nhi;
            }
        }
    } else {
        for (int x = 0; x < w; x++) {
            int rs = 0, gs = 0, bs = 0;
            int lo = 0;
            int hi = (r < h) ? r : (h - 1);
            for (int i = 0; i <= hi; i++) {
                const cc_rgb_t *p = src + (size_t)i * w + x;
                rs += p->r; gs += p->g; bs += p->b;
            }
            int win = hi + 1;
            for (int y = 0; y < h; y++) {
                cc_rgb_t *d = dst + (size_t)y * w + x;
                d->r = (uint8_t)(rs / win);
                d->g = (uint8_t)(gs / win);
                d->b = (uint8_t)(bs / win);

                int nlo = y + 1 - r; if (nlo < 0) nlo = 0;
                int nhi = y + 1 + r; if (nhi >= h) nhi = h - 1;
                if (nhi > hi) {
                    const cc_rgb_t *p = src + (size_t)nhi * w + x;
                    rs += p->r; gs += p->g; bs += p->b; win++;
                }
                if (nlo > lo) {
                    const cc_rgb_t *p = src + (size_t)lo * w + x;
                    rs -= p->r; gs -= p->g; bs -= p->b; win--;
                }
                lo = nlo; hi = nhi;
            }
        }
    }
}

/* Bloom the snapshot into s_blur_data (PSRAM).  TAKES OWNERSHIP of @p snap
 * (frees it) once converted.  Only THREE RGB888 buffers are ever live
 * (~1.85 MB peak) — the board has only ~2.7 MB PSRAM free, so the earlier
 * 4-buffer build failed and fell back to solid black. */
static bool cc_blur_snapshot(lv_draw_buf_t *snap)
{
    int w = (int)snap->header.w;
    int h = (int)snap->header.h;
    size_t n = (size_t)w * h;

    if (s_blur_data) { heap_caps_free(s_blur_data); s_blur_data = NULL; }

    /* A = original (later reused for the frosted base / final image). */
    cc_rgb_t *A = heap_caps_malloc(n * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!A) { lv_draw_buf_destroy(snap); return false; }

    /* Convert the RGB565 snapshot (stride-aware) straight into A, then
     * free the LVGL draw buffer so it doesn't eat PSRAM during the blur. */
    const uint8_t *src = (const uint8_t *)snap->data;
    uint32_t stride = snap->header.stride;
    for (int y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)(src + (size_t)y * stride);
        cc_rgb_t *dst = A + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint16_t p = row[x];
            dst[x].r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
            dst[x].g = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
            dst[x].b = (uint8_t)((p & 0x1F) * 255 / 31);
        }
    }
    lv_draw_buf_destroy(snap);

    cc_rgb_t *G = heap_caps_malloc(n * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); /* glow */
    cc_rgb_t *B = heap_caps_malloc(n * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); /* temp */
    if (!G || !B) {
        if (G) heap_caps_free(G);
        if (B) heap_caps_free(B);
        heap_caps_free(A);
        return false;
    }

    /* 1. glow source from the ORIGINAL (A): keep only NON-BLACK pixels,
     *    normalised up to full pop. */
    for (size_t i = 0; i < n; i++) {
        int luma = ((int)A[i].r + A[i].g + A[i].b) / 3;
        if (luma <= CC_BLOOM_THRESH) { G[i].r = G[i].g = G[i].b = 0; continue; }
        int mx = A[i].r;
        if (A[i].g > mx) mx = A[i].g;
        if (A[i].b > mx) mx = A[i].b;
        int k = (mx > 0) ? ((int)CC_BLOOM_AMP * 255 / mx) : 0;
        if (k > 255) k = 255;
        G[i].r = (uint8_t)(A[i].r * k / 255);
        G[i].g = (uint8_t)(A[i].g * k / 255);
        G[i].b = (uint8_t)(A[i].b * k / 255);
    }

    /* 2. frosted base: heavy blur of the original (A) → A (B is the temp). */
    int r = CC_BLUR_R;
    cc_box_pass(B, A, w, h, r, true);
    cc_box_pass(A, B, w, h, r, false);
    cc_box_pass(B, A, w, h, r, true);
    cc_box_pass(A, B, w, h, r, false);      /* base in A */

    /* 3. spread the glow far wider than the base — black never spreads. */
    int rg = CC_BLOOM_R;
    cc_box_pass(B, G, w, h, rg, true);
    cc_box_pass(G, B, w, h, rg, false);
    cc_box_pass(B, G, w, h, rg, true);
    cc_box_pass(G, B, w, h, rg, false);     /* wide glow in G */

    /* 4. composite: frosted base (A) + bright wide glow (G), clamped. */
    for (size_t i = 0; i < n; i++) {
        int gr = (int)G[i].r * CC_BLOOM_ADD / 255;
        int gg = (int)G[i].g * CC_BLOOM_ADD / 255;
        int gb = (int)G[i].b * CC_BLOOM_ADD / 255;
        int nr = (int)A[i].r + gr; A[i].r = (nr > 255) ? 255 : (uint8_t)nr;
        int ng = (int)A[i].g + gg; A[i].g = (ng > 255) ? 255 : (uint8_t)ng;
        int nb = (int)A[i].b + gb; A[i].b = (nb > 255) ? 255 : (uint8_t)nb;
    }

    /* 5. pack to RGB565 only now (only A is still live). */
    heap_caps_free(G);
    heap_caps_free(B);
    s_blur_data = heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_blur_data) { heap_caps_free(A); return false; }
    cc_rgb888_to_565(A, (uint16_t *)s_blur_data, (int)n);
    heap_caps_free(A);

    memset(&s_blur_dsc, 0, sizeof(s_blur_dsc));
    s_blur_dsc.header.w  = w;
    s_blur_dsc.header.h  = h;
    s_blur_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_blur_dsc.data_size = (uint32_t)n * 2;
    s_blur_dsc.data      = s_blur_data;
    return true;
}

/* ── Toggle / jump helpers ─────────────────────────────────────────── */

static bool cc_btn_active(cc_btn_t t)
{
    switch (t) {
    case CC_BTN_WIFI:       return app_wifi_is_connected();
    case CC_BTN_BT:         return g_config.ble_enabled;
    case CC_BTN_SILENT:     return g_config.silent_mode;
    case CC_BTN_AIRPLANE:   return g_config.airplane_mode;
    case CC_BTN_FLASHLIGHT: return false;   /* opens a separate screen */
    case CC_BTN_AOD:        return g_config.always_on_display;
    case CC_BTN_LOW_POWER:  return g_config.low_power_mode;
    case CC_BTN_SETTINGS:   /* fallthrough */
    case CC_BTN_REBOOT:     return false;   /* one-shot action buttons */
    default:                return false;
    }
}

/* Enabling an individual radio while airplane mode is on cancels airplane
 * mode (it had forced every radio off) and turns the radios back on. */
static void cc_cancel_airplane(void)
{
    if (!g_config.airplane_mode) return;
    g_config.airplane_mode = false;
    if (!g_config.wifi_enabled) g_config.wifi_enabled = true;
    if (!g_config.ble_enabled)  { g_config.ble_enabled = true;  g_toggle_ble = true; }
    ESP_LOGI(TAG, "Airplane cancelled");
}

static void cc_toggle_wifi(void)
{
    if (!g_config.wifi_enabled) {
        cc_cancel_airplane();               /* enabling cancels airplane */
        g_config.wifi_enabled = true;
    } else {
        g_config.wifi_enabled = false;
        app_wifi_set_enabled(false);
    }
    app_config_save();
    ESP_LOGI(TAG, "WiFi: %s", g_config.wifi_enabled ? "ON" : "OFF");
}

static void cc_toggle_ble(void)
{
    if (!g_config.ble_enabled) {
        cc_cancel_airplane();               /* enabling cancels airplane */
        g_config.ble_enabled = true;
    } else {
        g_config.ble_enabled = false;
    }
    g_toggle_ble = true;
    app_config_save();
    ESP_LOGI(TAG, "BLE: %s", g_config.ble_enabled ? "ON" : "OFF");
}

static void cc_toggle_silent(void)
{
    bool on = !g_config.silent_mode;
    if (!on) app_events_request_vibe_click();
    g_config.silent_mode = on;
    hw_vibrator_set_muted(on);
    app_config_save();
    ESP_LOGI(TAG, "Silent: %s", on ? "ON" : "OFF");
}

static void cc_toggle_airplane(void)
{
    bool on = !g_config.airplane_mode;
    g_config.airplane_mode = on;
    if (on) {
        g_config.wifi_enabled = false;
        app_wifi_set_enabled(false);
        g_config.ble_enabled = false;
        g_toggle_ble = true;
        g_config.ancs_enabled = false;
        g_toggle_ancs = true;
    } else {
        g_config.wifi_enabled = true;
        g_config.ble_enabled = true;
        g_toggle_ble = true;
    }
    app_config_save();
    ESP_LOGI(TAG, "Airplane: %s", on ? "ON" : "OFF");
}

static void cc_toggle_aod(void)
{
    bool on = !g_config.always_on_display;
    g_config.always_on_display = on;
    app_power_set_aod_enabled(on);
    app_config_save();
    ESP_LOGI(TAG, "AOD: %s", on ? "ON" : "OFF");
}

static void cc_toggle_low_power(void)
{
    app_power_set_low_power(!g_config.low_power_mode);
}

/* ── Button click ──────────────────────────────────────────────────── */

static void cc_btn_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    cc_btn_t t = s_items[idx].type;
    app_events_request_vibe_click();

    switch (t) {
    case CC_BTN_WIFI:       cc_toggle_wifi();        break;
    case CC_BTN_BT:         cc_toggle_ble();         break;
    case CC_BTN_SILENT:     cc_toggle_silent();      break;
    case CC_BTN_FIND:       /* placeholder */         break;
    case CC_BTN_DND:        /* placeholder */         break;
    case CC_BTN_AIRPLANE:   cc_toggle_airplane();    break;
    case CC_BTN_FLASHLIGHT: g_pending_cc_flashlight = true; break;
    /* Brightness jumps to Settings > Display (brightness slider is top). */
    case CC_BTN_BRIGHTNESS: g_pending_cc_settings_target = SETTINGS_CC_DISPLAY; break;
    case CC_BTN_VOLUME:     g_pending_cc_settings_target = SETTINGS_CC_VOLUME;  break;
    case CC_BTN_AOD:        cc_toggle_aod();         break;
    case CC_BTN_LOW_POWER:  cc_toggle_low_power();   break;
    case CC_BTN_SETTINGS:   g_pending_cc_open_settings = true; break;
    case CC_BTN_REBOOT:     app_power_reboot();      break;  /* graceful reboot */
    case CC_BTN_BATTERY:    g_pending_cc_settings_target = SETTINGS_CC_BATTERY; break;
    default: break;
    }

    cc_refresh_states();
}

/* ── Per-button style update ───────────────────────────────────────── */

static void cc_update_button(int idx)
{
    cc_item_t *it = &s_items[idx];
    if (!it->obj || !lv_obj_is_valid(it->obj)) return;

    bool active = cc_btn_active(it->type);
    uint32_t bg = CC_INACTIVE_BG;
    if (active) {
        switch (it->type) {
        case CC_BTN_SILENT:    bg = CC_SILENT_BG;    break;
        case CC_BTN_LOW_POWER: bg = CC_LOW_POWER_BG; break; /* orange-yellow */
        default:               bg = CC_ACTIVE_BG;    break;
        }
    }

    lv_obj_set_style_bg_color(it->obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(it->obj, LV_OPA_COVER, 0);

    if (it->icon && lv_obj_is_valid(it->icon)) {
        lv_obj_set_style_text_color(it->icon,
            lv_color_hex(active ? CC_ICON_ON : CC_ICON_OFF), 0);
    }
    if (it->label && lv_obj_is_valid(it->label)) {
        /* Battery text is always white; the AOD label follows the toggle. */
        uint32_t col = (it->type == CC_BTN_BATTERY) ? 0xFFFFFF
                      : (active ? CC_ICON_ON : CC_ICON_OFF);
        lv_obj_set_style_text_color(it->label, lv_color_hex(col), 0);
    }
}

static void cc_update_battery(void)
{
    for (int i = 0; i < CC_BTN_COUNT; i++) {
        if (s_items[i].type != CC_BTN_BATTERY) continue;
        if (s_items[i].label && lv_obj_is_valid(s_items[i].label)) {
            lv_label_set_text_fmt(s_items[i].label, "%d %%", g_battery_pct);
        }
        break;
    }
}

/* ── Status bar ────────────────────────────────────────────────────── */

static uint8_t cc_status_icon_set(void)
{
    uint8_t set = 0;
    if (app_wifi_is_connected())      set |= ST_WIFI;
    if (g_ancs_connected)             set |= ST_PHONE;
    /* "Charging" status = USB is physically connected OR charge current is
     * flowing (a full battery that is plugged in still counts). */
    if (g_is_vbus_present || g_is_charging)  set |= ST_CHARGING;
    if (g_config.silent_mode)         set |= ST_SILENT;
    return set;
}

static void cc_status_destroy(void)
{
    if (s_status && lv_obj_is_valid(s_status)) lv_obj_delete(s_status);
    s_status = NULL;
}

/* Bit 0..3 → wifi, phone(BLE), charging, silent */
static const char *const s_st_icons[4] = {
    SW_ICON_WIFI, SW_ICON_BLUETOOTH, MI_BATTERY_CHARGING, SW_ICON_VOLUME_OFF
};
static const uint32_t s_st_colors[4] = { 0x4892F1, 0x2094FA, 0x04DE71, 0xFF3B30 };

static int cc_status_width(uint8_t set)
{
    int n = 0;
    for (int i = 0; i < 4; i++) if (set & (1 << i)) n++;
    if (n == 0) return 0;
    int content = n * CC_ICON_SLOT + (n - 1) * 11;
    int w = content + 2 * 20;
    return (w < 40) ? 40 : w;
}

/* Rebuild the icon children of the existing pill for the current set. */
static void cc_status_fill(void)
{
    if (!s_status || !lv_obj_is_valid(s_status)) return;
    uint8_t set = s_status_set;
    lv_obj_clean(s_status);

    int n = 0;
    for (int i = 0; i < 4; i++) if (set & (1 << i)) n++;
    if (n == 0) return;

    int content = n * CC_ICON_SLOT + (n - 1) * 11;
    int x_off = -(content / 2) + CC_ICON_SLOT / 2;
    int placed = 0;
    for (int i = 0; i < 4; i++) {
        if (!(set & (1 << i))) continue;
        lv_obj_t *ic = lv_label_create(s_status);
        lv_label_set_text(ic, s_st_icons[i]);
        lv_obj_set_style_text_font(ic, SW_ICON_F, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(s_st_colors[i]), 0);
        lv_obj_set_style_opa(ic, LV_OPA_COVER, 0);
        lv_obj_set_size(ic, CC_ICON_SLOT, CC_ICON_SLOT);
        lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(ic, LV_ALIGN_CENTER, x_off + placed * 36, 0);
        placed++;
    }
}

/* Create / update the status pill.  A width change animates width + x so
 * the pill always stays horizontally centred while it grows/shrinks. */
static void cc_status_update(void)
{
    uint8_t set = s_status_set;
    int w = cc_status_width(set);

    if (w == 0) { cc_status_destroy(); return; }

    if (!s_status) {
        s_status = lv_obj_create(s_scr);
        lv_obj_set_size(s_status, w, CC_STATUS_H);
        lv_obj_set_pos(s_status, (CC_W - w) / 2, CC_STATUS_Y);
        lv_obj_set_style_radius(s_status, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_status, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(s_status, 100, 0);
        lv_obj_set_style_border_width(s_status, 0, 0);
        lv_obj_remove_flag(s_status, LV_OBJ_FLAG_SCROLLABLE);
        cc_status_fill();
        return;
    }

    int old_w = lv_obj_get_width(s_status);
    int old_x = lv_obj_get_x(s_status);
    int new_x = (CC_W - w) / 2;
    cc_status_fill();

    if (old_w != w) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_status);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_set_values(&a, old_w, w);
        lv_anim_set_time(&a, 250);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    if (old_x != new_x) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_status);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
        lv_anim_set_values(&a, old_x, new_x);
        lv_anim_set_time(&a, 250);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

/* ── Grid build ────────────────────────────────────────────────────── */

static void cc_build_button(int idx)
{
    cc_item_t *it = &s_items[idx];
    it->type = s_order[idx];
    it->full = false;
    it->right = (idx % 2) == 1;

    int row = idx / 2;
    int x = it->right ? CC_RIGHT_X : CC_LEFT_X;
    int w = CC_BTN_W;
    int y = row * CC_ROW_PITCH;

    lv_obj_t *btn = lv_button_create(s_grid);
    lv_obj_set_size(btn, w, CC_BTN_H);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    /* Solid fill (no transparency — 250 vs 255 was imperceptible). */
    lv_obj_set_style_bg_color(btn, lv_color_hex(CC_INACTIVE_BG), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    /* transform pivot: inner edge fixed, so the center gap never changes */
    if (it->right) {
        lv_obj_set_style_transform_pivot_x(btn, lv_pct(0), 0);
    } else {
        lv_obj_set_style_transform_pivot_x(btn, lv_pct(100), 0);
    }
    lv_obj_set_style_transform_pivot_y(btn, lv_pct(50), 0);

    /* Press feedback (LVGL native state): a quick scale pop while held,
     * animated by the style anim-duration the scroll timer sets (~100 ms). */
    lv_obj_set_style_transform_scale(btn, 272, LV_STATE_PRESSED);  /* ~106% */

    if (it->type == CC_BTN_BATTERY || it->type == CC_BTN_AOD) {
        /* Text buttons — "100 %" battery and "AOD" toggle (battery style). */
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &noto_font_48_bold, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_opa(lbl, LV_OPA_COVER, 0);
        lv_label_set_text(lbl, (it->type == CC_BTN_BATTERY) ? "100 %" : "AOD");
        lv_obj_center(lbl);
        it->label = lbl;
    } else {
        lv_obj_t *ic = lv_label_create(btn);
        lv_label_set_text(ic, s_icons[it->type]);
        lv_obj_set_style_text_font(ic, &mi_font_60, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(CC_ICON_OFF), 0);
        lv_obj_set_style_opa(ic, LV_OPA_COVER, 0);   /* never dimmed by button */
        lv_obj_center(ic);
        it->icon = ic;
    }

    lv_obj_add_event_cb(btn, cc_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
    it->obj = btn;
}

static void cc_build_grid(void)
{
    uint8_t set = cc_status_icon_set();
    s_status_set = set;
    int top_pad = (set != 0) ? CC_TOP_WITH_STATUS : CC_TOP_NO_STATUS;

    s_grid = lv_obj_create(s_scr);
    lv_obj_set_size(s_grid, CC_W, CC_H);
    lv_obj_set_pos(s_grid, 0, 0);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_style_pad_top(s_grid, top_pad, 0);
    lv_obj_set_style_pad_bottom(s_grid, CC_BOTTOM_PAD, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < CC_BTN_COUNT; i++) cc_build_button(i);
}

/* ── Refresh all button + status visuals ───────────────────────────── */

static void cc_refresh_states(void)
{
    if (!s_active) return;

    uint8_t set = cc_status_icon_set();
    if (set != s_status_set) {
        s_status_set = set;
        int top_pad = (set != 0) ? CC_TOP_WITH_STATUS : CC_TOP_NO_STATUS;
        if (s_grid && lv_obj_is_valid(s_grid)) {
            lv_obj_set_style_pad_top(s_grid, top_pad, 0);
        }
        cc_status_update();
    }

    for (int i = 0; i < CC_BTN_COUNT; i++) cc_update_button(i);
    cc_update_battery();
}

/* ── Scroll-scale + corner-fit timer ───────────────────────────────── */

static void cc_scroll_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active || !s_grid) return;

    /* Gentle bottom-only scroll scale keyed to the button TOP edge: a button
     * whose top is at (or below) the screen bottom is 60%, easing up to 100%
     * once its top is H/5 above the bottom.  Fully-visible rows stay 100%. */
    lv_area_t va;
    lv_obj_get_coords(s_grid, &va);
    int vp_bottom = va.y2;
    int fade_zone = CC_H / 5;               /* bottom fifth of the screen */
    if (fade_zone < 40) fade_zone = 40;

    for (int i = 0; i < CC_BTN_COUNT; i++) {
        lv_obj_t *o = s_items[i].obj;
        if (!o || !lv_obj_is_valid(o)) continue;

        lv_area_t a;
        lv_obj_get_coords(o, &a);
        int y_top = a.y1;

        int dist = vp_bottom - y_top;       /* 0 when the top edge is at the bottom */
        if (dist < 0) dist = 0;
        if (dist > fade_zone) dist = fade_zone;

        int tval = dist * 256 / fade_zone;
        int s = 154 + (256 - 154) * tval / 256;   /* 60% → 100% */
        if (s < 154) s = 154;
        if (s > 256) s = 256;

        if (lv_obj_get_style_transform_scale_x(o, 0) != s) {
            lv_obj_set_style_transform_scale(o, s, 0);
        }
        if (lv_obj_get_style_anim_duration(o, 0) != 100) {
            lv_obj_set_style_anim_duration(o, 100, 0);
        }
    }
}

static void cc_state_timer_cb(lv_timer_t *t)
{
    (void)t;
    cc_refresh_states();
}

/* ── Public API ────────────────────────────────────────────────────── */

void control_center_init(void)
{
    ESP_LOGI(TAG, "Control center ready");
}

bool control_center_is_active(void)
{
    return s_active;
}

void control_center_open(void)
{
    if (s_active) return;

    s_prev_scr = lv_screen_active();
    if (!s_prev_scr) return;

    /* 1. one-time heavy blur of the current screen
     * (cc_blur_snapshot takes ownership of the snapshot draw buffer). */
    lv_draw_buf_t *snap = lv_snapshot_take(s_prev_scr, LV_COLOR_FORMAT_RGB565);
    if (snap) {
        if (!cc_blur_snapshot(snap)) {
            ESP_LOGW(TAG, "Blur failed — falling back to solid black");
        }
    } else {
        ESP_LOGW(TAG, "Snapshot failed — falling back to solid black");
    }

    /* 2. screen */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, CC_W, CC_H);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* blurred backdrop */
    if (s_blur_data) {
        lv_obj_t *bg = lv_image_create(s_scr);
        lv_image_set_src(bg, &s_blur_dsc);
        lv_obj_set_pos(bg, 0, 0);
        lv_obj_set_size(bg, CC_W, CC_H);
    }

    /* 3. grid + status bar */
    cc_build_grid();
    cc_status_update();

    /* 4. load */
    lv_screen_load(s_scr);
    s_active = true;

    cc_refresh_states();

    s_scroll_timer = lv_timer_create(cc_scroll_timer_cb, 50, NULL);
    lv_timer_set_repeat_count(s_scroll_timer, -1);
    s_state_timer = lv_timer_create(cc_state_timer_cb, 500, NULL);
    lv_timer_set_repeat_count(s_state_timer, -1);

    ESP_LOGI(TAG, "Opened");
}

void control_center_close(void)
{
    if (!s_active) return;
    s_active = false;

    if (s_scroll_timer) { lv_timer_del(s_scroll_timer); s_scroll_timer = NULL; }
    if (s_state_timer)  { lv_timer_del(s_state_timer);  s_state_timer = NULL; }


    /* restore the previous screen (CC screen removed) */
    if (s_prev_scr && lv_obj_is_valid(s_prev_scr)) {
        lv_screen_load(s_prev_scr);
    }
    if (s_scr && lv_obj_is_valid(s_scr)) lv_obj_delete(s_scr);

    /* free the blurred backdrop */
    if (s_blur_data) { heap_caps_free(s_blur_data); s_blur_data = NULL; }
    memset(&s_blur_dsc, 0, sizeof(s_blur_dsc));

    s_scr = NULL; s_grid = NULL; s_status = NULL; s_prev_scr = NULL;
    memset(s_items, 0, sizeof(s_items));

    ESP_LOGI(TAG, "Closed");
}

void control_center_handle_back(void)
{
    if (!s_active) return;
    app_events_request_vibe_click();
    control_center_close();
}

/* ── GPIO10 at top: rubber-band bounce ─────────────────────────────── */

static void cc_bounce_exec_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}

static void cc_bounce_back_start(lv_anim_t *a)
{
    lv_obj_t *grid = lv_anim_get_user_data(a);
    if (!grid || !lv_obj_is_valid(grid)) return;
    lv_anim_t back;
    lv_anim_init(&back);
    lv_anim_set_var(&back, grid);
    lv_anim_set_time(&back, 300);
    lv_anim_set_values(&back, CC_BOUNCE_DIST, 0);
    lv_anim_set_path_cb(&back, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&back, cc_bounce_exec_cb);
    lv_anim_start(&back);
}

void control_center_scroll_top(void)
{
    if (!s_active || !s_grid || !lv_obj_is_valid(s_grid)) return;

    /* GPIO10 always clicks — even when there is nothing to scroll. */
    app_events_request_vibe_click();

    if (lv_obj_get_scroll_y(s_grid) > 0) {
        lv_obj_scroll_to_y(s_grid, 0, LV_ANIM_ON);
        return;
    }

    /* Already at the top → elastic bounce so the press has feedback. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_grid);
    lv_anim_set_user_data(&a, s_grid);
    lv_anim_set_time(&a, 130);
    lv_anim_set_values(&a, 0, CC_BOUNCE_DIST);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, cc_bounce_exec_cb);
    lv_anim_set_ready_cb(&a, cc_bounce_back_start);
    lv_anim_start(&a);
}
