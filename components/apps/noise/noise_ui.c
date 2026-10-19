/**
 * @file noise_ui.c
 * @brief Noise (dB meter) system app — audio capture + dynamic UI.
 *
 * A faithful port of the user's SquareLine "Noise" design (ui/ui_noise.c)
 * wired to the two onboard microphones via the BSP codec layer:
 *
 *   - The mic (ES7210) is opened once at boot by hw_audio_init() through the
 *     BSP API `bsp_audio_codec_microphone_init()`.  The Noise app reads PCM
 *     from that already-open handle (`hw_audio_get_mic()`), so it never
 *     re-configures the shared I2S / codec and cannot disturb the OS.
 *
 *   - A dedicated FreeRTOS task reads 50 ms blocks, computes the RMS and
 *     converts it to dB SPL (20*log10(rms/FS) + offset).  A 100 ms LVGL
 *     timer picks up the newest dB value and refreshes the widgets.
 *
 * Behaviour (per spec):
 *   - Theme colour follows dB:  green (< 80) → yellow (80–94) → red (≥ 95).
 *   - Status word:              OK → Loud → Harmful.
 *   - Status symbol:            LV_SYMBOL_OK → LV_SYMBOL_WARNING at ≥ 95 dB.
 *   - 20 level bars:            bar 1 = 30 dB, bar 12 = 80 dB, bar 20 = 120 dB.
 *     A bar turns C4A818 → FFFF00 once the current dB reaches its threshold.
 *   - The "db" unit label is re-positioned with the digit count so it never
 *     overlaps or leaves a gap when the value flips between 2 and 3 digits.
 *
 * Memory model: every app-owned buffer (context, audio block) is PSRAM and
 * is freed on destroy(); the UI widgets use the PSRAM-backed LVGL allocator.
 */

#include "noise_ui.h"

#include "ui/ui_noise.h"
#include "hw_audio.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "NOISE";

/* ── Screen (matches the 410x502 AMOLED) ──────────────────────────── */
#define NOISE_W   410
#define NOISE_H   502

/* ── Audio capture ────────────────────────────────────────────────────
 * The mic is already open at 16 kHz / mono / 16-bit (see hw_audio_init).
 * One "block" = 50 ms of PCM, read by the audio task every cycle.       */
#define NOISE_SAMPLE_RATE    16000
#define NOISE_BLOCK_MS       50
#define NOISE_BLOCK_SAMPLES  (NOISE_SAMPLE_RATE * NOISE_BLOCK_MS / 1000) /* 800 */
#define NOISE_BLOCK_BYTES    (NOISE_BLOCK_SAMPLES * 2)                    /* 1600 */
#define NOISE_DB_OFFSET      96.0f   /* dBFS → dB SPL calibration offset */

#define NOISE_TASK_NAME      "noise"
#define NOISE_TASK_STACK     4096
#define NOISE_TASK_PRIO      5

/* ── Theme / status thresholds (dB SPL) ─────────────────────────────── */
#define NOISE_THEME_YELLOW_DB   80    /* ≥ 80  → yellow theme  */
#define NOISE_THEME_RED_DB       95    /* ≥ 95  → red theme     */

/* ── Level bars: 20 bars, 30 dB → 120 dB ──────────────────────────────
 * Bar 1 = 30 dB, bar 12 = 80 dB, bar 20 = 120 dB.  The first 11 bars step
 * (80-30)/11 ≈ 4.545 dB, the last 8 step 5 dB.                         */
#define NOISE_BAR_COUNT   20
#define NOISE_BAR_MIN_DB  30.0f
#define NOISE_BAR_12_DB   80.0f
#define NOISE_BAR_MAX_DB  120.0f

#define NOISE_GRAD_STOPS   8    /* stops in the smoothed bg gradient  */

#define NOISE_GRAD_STOPS   8    /* stops in the smoothed bg gradient  */

/* ── Themes (hue-shifted from the yellow reference design) ──────────── */
typedef struct {
    uint32_t bg_top;        /* screen gradient top          */
    uint32_t bg_bottom;     /* screen gradient bottom       */
    uint32_t accent;        /* status word + symbol         */
    uint32_t db_label;      /* 30 / 80 / 120 labels         */
    uint32_t bar_inactive;  /* level bar — threshold not met */
    uint32_t bar_active;    /* level bar — threshold met    */
} noise_theme_t;

static const noise_theme_t s_themes[] = {
    /* GREEN  — db < 80                    */
    { 0x1E9E4E, 0x0A3D1C, 0x4DFF8A, 0xB7F7D0, 0x0C5C2B, 0x4DFF8A },
    /* YELLOW — 80 ≤ db < 95 (reference)    */
    { 0xD3B20E, 0x463A01, 0xFFFF00, 0xFCEF8C, 0xC4A818, 0xFFFF00 },
    /* RED    — db ≥ 95                     */
    { 0xE03A2E, 0x4A0E08, 0xFF5C4D, 0xFFB3AB, 0x7A160E, 0xFF5C4D },
};

/* Status word per theme index (0 = OK, 1 = Loud, 2 = Harmful). */
static const char *s_status_str[] = { "OK", "Loud", "Harmful" };

/* ── App context (allocated from PSRAM on create) ───────────────────── */
typedef struct {
    lv_obj_t   *scr;                     /* ui_noise screen             */
    lv_timer_t *timer;                   /* LVGL UI refresh timer       */
    TaskHandle_t task;                   /* audio sampling task         */
    esp_codec_dev_handle_t mic;          /* OS-opened mic handle        */
    int16_t    *audio_buf;               /* PCM block (PSRAM)           */
    volatile float db;                   /* latest dB from audio task   */
    volatile bool  run;                  /* audio task keep-alive flag  */
    int          last_db;                /* last displayed dB (int)     */
    int          last_theme;             /* last applied theme index    */
    lv_obj_t    *bars[NOISE_BAR_COUNT];  /* bar widgets (from ui_noise) */
    lv_obj_t    *prev_screen;            /* screen to restore on exit   */
    lv_grad_dsc_t bg_grad;               /* smoothed bg gradient        */
} noise_ctx_t;

static noise_ctx_t *s_ctx = NULL;

/* ══════════════════════════════════════════════════════════════════════
 * Level-bar threshold (dB) for bar index (0-based; bar 1 == index 0).
 * ══════════════════════════════════════════════════════════════════════ */
static float noise_bar_threshold(int idx)
{
    if (idx < 11) {
        return NOISE_BAR_MIN_DB + idx * ((NOISE_BAR_12_DB - NOISE_BAR_MIN_DB) / 11.0f);
    }
    if (idx == 11) {
        return NOISE_BAR_12_DB;
    }
    return NOISE_BAR_12_DB + (idx - 11) * 5.0f;
}

/* ══════════════════════════════════════════════════════════════════════
 * Audio task — reads PCM, computes RMS → dB, stores the newest value.
 * Runs for the whole lifetime of the app; exits itself on destroy().
 * ══════════════════════════════════════════════════════════════════════ */
static void noise_audio_task(void *arg)
{
    noise_ctx_t *c = (noise_ctx_t *)arg;

    while (c->run) {
        /* Blocking read — returns once a full 50 ms block is available
         * (the I2S capture runs continuously from boot).  ~100 ms max. */
        int ret = esp_codec_dev_read(c->mic, c->audio_buf, NOISE_BLOCK_BYTES);
        if (!c->run) break;
        if (ret != ESP_CODEC_DEV_OK) continue;

        /* RMS over the block */
        const int16_t *s = c->audio_buf;
        double acc = 0.0;
        for (int i = 0; i < NOISE_BLOCK_SAMPLES; i++) {
            double v = s[i];
            acc += v * v;
        }
        double rms = sqrt(acc / (double)NOISE_BLOCK_SAMPLES);

        /* dBFS → dB SPL, clamped to the meter range */
        float db = 20.0f * log10f((float)(rms / 32768.0)) + NOISE_DB_OFFSET;
        if (db < 0.0f)             db = 0.0f;
        if (db > NOISE_BAR_MAX_DB) db = NOISE_BAR_MAX_DB;
        c->db = db;
    }

    c->task = NULL;               /* signal destroy() that we are done */
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * Background gradient — smooth multi-stop vertical gradient.
 *
 * The generated 2-stop gradient interpolates in linear RGB, which looks
 * muddy and banded at 16-bit.  Here the top/bottom theme colours are
 * blended with a gamma-corrected (≈2.2) curve across NOISE_GRAD_STOPS
 * stops, giving a soft, blur-like transition with no visible stepping.
 * ══════════════════════════════════════════════════════════════════════ */
static void noise_build_bg_grad(noise_ctx_t *c, const noise_theme_t *th)
{
    lv_grad_dsc_t *g = &c->bg_grad;
    memset(g, 0, sizeof(*g));
    g->dir = LV_GRAD_DIR_VER;
    g->extend = LV_GRAD_EXTEND_PAD;
    g->stops_count = NOISE_GRAD_STOPS;

    const float tr = (th->bg_top >> 16) & 0xFF;
    const float tg = (th->bg_top >> 8) & 0xFF;
    const float tb = th->bg_top & 0xFF;
    const float br = (th->bg_bottom >> 16) & 0xFF;
    const float bg = (th->bg_bottom >> 8) & 0xFF;
    const float bb = th->bg_bottom & 0xFF;

    for (int i = 0; i < NOISE_GRAD_STOPS; i++) {
        float f = (float)i / (float)(NOISE_GRAD_STOPS - 1);
        uint8_t r  = (uint8_t)(powf(powf(tr, 2.2f) * (1.0f - f) + powf(br, 2.2f) * f, 1.0f / 2.2f) + 0.5f);
        uint8_t g2 = (uint8_t)(powf(powf(tg, 2.2f) * (1.0f - f) + powf(bg, 2.2f) * f, 1.0f / 2.2f) + 0.5f);
        uint8_t b  = (uint8_t)(powf(powf(tb, 2.2f) * (1.0f - f) + powf(bb, 2.2f) * f, 1.0f / 2.2f) + 0.5f);
        g->stops[i].color = lv_color_make(r, g2, b);
        g->stops[i].opa = LV_OPA_COVER;
        g->stops[i].frac = (uint8_t)(f * 255.0f);
    }

    lv_obj_set_style_bg_grad(ui_noise, g, 0);
    lv_obj_set_style_bg_color(ui_noise, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_grad_color(ui_noise, lv_color_hex(th->bg_bottom), 0);
    lv_obj_set_style_bg_grad_dir(ui_noise, LV_GRAD_DIR_VER, 0);
}

/* ══════════════════════════════════════════════════════════════════════
 * UI refresh — apply the latest dB value to every widget.
 * Only re-colours the theme when the theme band actually changes.
 * ══════════════════════════════════════════════════════════════════════ */
static void noise_update_ui(noise_ctx_t *c, int db)
{
    /* Theme band: 0 green, 1 yellow, 2 red */
    int theme = 0;
    if (db >= NOISE_THEME_RED_DB)          theme = 2;
    else if (db >= NOISE_THEME_YELLOW_DB)  theme = 1;
    const noise_theme_t *th = &s_themes[theme];

    if (theme != c->last_theme) {
        c->last_theme = theme;
        noise_build_bg_grad(c, th);
        lv_obj_set_style_text_color(ui_status, lv_color_hex(th->accent), 0);
        lv_obj_set_style_text_color(ui_status_symbol, lv_color_hex(th->accent), 0);
        lv_obj_set_style_text_color(ui_db30, lv_color_hex(th->db_label), 0);
        lv_obj_set_style_text_color(ui_db80, lv_color_hex(th->db_label), 0);
        lv_obj_set_style_text_color(ui_db120, lv_color_hex(th->db_label), 0);
    }

    /* Status symbol (icon) sits at the top-left of the status box and the
     * status word is placed right after it — sized dynamically so the OK /
     * WARNING symbols never overlap the word. */
    const char *sym = (db >= NOISE_THEME_RED_DB) ? LV_SYMBOL_WARNING : LV_SYMBOL_OK;
    lv_label_set_text(ui_status_symbol, sym);
    lv_label_set_text(ui_status, s_status_str[theme]);
    lv_point_t _txt_w;
    lv_text_get_size(&_txt_w, sym, &lv_font_montserrat_28, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t sym_w = _txt_w.x;
    lv_obj_set_x(ui_status, sym_w + 6);

    /* Current dB value */
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", db);
    lv_label_set_text(ui_status1, buf);

    /* Move the "db" unit so it tracks the digit width of the number —
     * no overlap/gap when flipping between 2 and 3 digits. */
    int32_t num_w;
    lv_point_t _txt_w2;
    lv_text_get_size(&_txt_w2, buf, &lv_font_montserrat_48, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    num_w = _txt_w2.x;
    lv_obj_set_x(ui_status2, num_w + 8);

    /* Level bars — follow the theme's active/inactive colours */
    for (int i = 0; i < NOISE_BAR_COUNT; i++) {
        lv_obj_t *bar = c->bars[i];
        if (!bar) continue;
        bool lit = ((float)db >= noise_bar_threshold(i));
        lv_obj_set_style_bg_color(bar, lv_color_hex(lit ? th->bar_active
                                                        : th->bar_inactive), 0);
    }
}

static void noise_timer_cb(lv_timer_t *t)
{
    noise_ctx_t *c = (noise_ctx_t *)lv_timer_get_user_data(t);
    if (!c || !c->scr) return;

    int db = (int)(c->db + 0.5f);           /* round to nearest dB */
    if (db < 0) db = 0;
    if (db > (int)NOISE_BAR_MAX_DB) db = (int)NOISE_BAR_MAX_DB;
    if (db == c->last_db) return;           /* nothing changed */
    c->last_db = db;

    noise_update_ui(c, db);
}

/* Info ("i") button — acts as an on-screen back/exit control. */
static void noise_info_cb(lv_event_t *e)
{
    (void)e;
    noise_ui_handle_back();
}

/* ══════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════ */
void noise_ui_create(void)
{
    if (s_ctx) {
        noise_ui_destroy();
    }

    /* App context — PSRAM */
    noise_ctx_t *c = (noise_ctx_t *)heap_caps_calloc(1, sizeof(noise_ctx_t),
                                                     MALLOC_CAP_SPIRAM);
    if (!c) {
        ESP_LOGE(TAG, "OOM: failed to allocate context in PSRAM");
        return;
    }
    s_ctx = c;

    /* Audio buffer — PSRAM (mic handle already open, owned by the OS) */
    c->mic = hw_audio_get_mic();
    if (!c->mic) {
        ESP_LOGE(TAG, "Mic not available — showing static UI");
    }
    c->audio_buf = (int16_t *)heap_caps_malloc(NOISE_BLOCK_BYTES, MALLOC_CAP_SPIRAM);
    if (!c->audio_buf) {
        ESP_LOGE(TAG, "OOM: failed to allocate audio buffer in PSRAM");
        noise_ui_destroy();
        return;
    }

    /* Build the SquareLine-ported screen */
    ui_noise_screen_init();
    c->scr = ui_noise;

    /* Collect the 20 bar widgets for dynamic lighting */
    lv_obj_t *bars[NOISE_BAR_COUNT] = {
        ui_noise_bar_1,  ui_noise_bar_2,  ui_noise_bar_3,  ui_noise_bar_4,
        ui_noise_bar_5,  ui_noise_bar_6,  ui_noise_bar_7,  ui_noise_bar_8,
        ui_noise_bar_9,  ui_noise_bar_10, ui_noise_bar_11, ui_noise_bar_12,
        ui_noise_bar_13, ui_noise_bar_14, ui_noise_bar_15, ui_noise_bar_16,
        ui_noise_bar_17, ui_noise_bar_18, ui_noise_bar_19, ui_noise_bar_20,
    };
    for (int i = 0; i < NOISE_BAR_COUNT; i++) {
        c->bars[i] = bars[i];
    }

    /* Start with the green theme, no bars lit */
    c->last_theme = -1;
    c->last_db = -1;
    noise_update_ui(c, 0);

    /* The "i" button in the design doubles as a back button */
    if (ui_more_information_) {
        lv_obj_add_event_cb(ui_more_information_, noise_info_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Remember the screen to restore on exit (the watch face — the menu is
     * only a child overlay of it), so destroy() can switch back cleanly. */
    c->prev_screen = lv_screen_active();
    lv_screen_load(c->scr);

    /* Start mic sampling task */
    if (c->mic && c->audio_buf) {
        c->run = true;
        if (xTaskCreate(noise_audio_task, NOISE_TASK_NAME, NOISE_TASK_STACK,
                        c, NOISE_TASK_PRIO, &c->task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio task");
            c->run = false;
        }
    }

    /* LVGL refresh timer (100 ms) */
    c->timer = lv_timer_create(noise_timer_cb, 100, c);

    ESP_LOGI(TAG, "Noise UI created (mic %s)",
             c->mic ? "ready" : "unavailable");
}

void noise_ui_destroy(void)
{
    if (!s_ctx) return;
    noise_ctx_t *c = s_ctx;
    s_ctx = NULL;

    /* Stop the audio task.  It checks c->run right after the (≤ ~100 ms)
     * blocking read, so a short wait guarantees it has exited before we
     * free its PSRAM audio buffer. */
    c->run = false;
    if (c->task) {
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (c->timer) {
        lv_timer_delete(c->timer);
        c->timer = NULL;
    }

    if (c->scr) {
        if (c->prev_screen && lv_obj_is_valid(c->prev_screen)) {
            /* Restore the previous screen (watch face) with a fade, exactly
             * like settings/calculator.  auto_del=true lets LVGL free the
             * noise screen after the animation.  Deleting the active screen
             * directly left the display pointing at freed memory, which made
             * the LVGL refresh timer spin on it → task watchdog. */
            ui_noise = NULL;   /* screen_destroy() below must only clear ptrs */
            lv_screen_load_anim(c->prev_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, true);
            ui_noise_screen_destroy();   /* ui_noise==NULL → skips delete */
        } else {
            /* No previous screen: delete directly and NULL the pointers. */
            ui_noise_screen_destroy();
        }
        c->scr = NULL;
    }

    if (c->audio_buf) {
        heap_caps_free(c->audio_buf);
        c->audio_buf = NULL;
    }

    heap_caps_free(c);
    ESP_LOGI(TAG, "Noise UI destroyed (PSRAM released)");
}

bool noise_ui_is_active(void)
{
    return s_ctx != NULL;
}

void noise_ui_handle_back(void)
{
    noise_ui_destroy();
}
