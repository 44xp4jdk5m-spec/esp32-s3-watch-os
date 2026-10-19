/**
 * @file ui_boot_up.c
 * @brief Boot-up splash screen using the shared radial progress ring.
 *
 * A black screen with a centred "Watch OS" title and a ring of discrete
 * radial bars (idle gray, active white) that light up clockwise from the
 * top as progress advances.  The ring is the shared ui_progress_ring widget
 * (also used by the OTA install screen), sized to meet the screen edge.
 * Progress ramps up slowly on a timer because the boot duration is unknown,
 * and is jumped forward by ui_boot_up_bump_progress() when a boot milestone
 * passes (e.g. WiFi got an IP).  ui_boot_up_complete() forces the ring to
 * 100% so every bar is lit before the screen is swapped out.
 */

#include "ui_boot_up.h"
#include "ui_progress_ring.h"
#include "esp_log.h"

static const char *TAG = "UI_BOOT";

/* Ring meets the edge: 410px diameter = the 410px screen width. */
#define RING_SIZE           410
#define RING_BARS           24           /* ElenixOS-style: few, clearly
                                          * individual bars so the ring reads
                                          * as bars turning white clockwise
                                          * (not a solid sweep). */
#define RING_BAR_WIDTH      6            /* thicker bars for the large ring */

/* Ramp fills the whole ring in ~2 s when nothing else jumps progress
 * forward (e.g. WiFi disabled — see app_wifi.c): 25 units per 50 ms
 * tick = 2.5 %/tick = 50 %/s.  Boot milestones still jump ahead. */
#define RAMP_DELTA          25          /* g_progress units per tick (0.1%) */
#define RAMP_MAX          1000          /* g_progress range: 0..1000 = 0..100% */
#define ANIM_PERIOD_MS      50

/* ── Screen objects ─────────────────────────────────────────────────── */

lv_obj_t *ui_boot_up      = NULL;
lv_obj_t *ui_boot_label   = NULL;   /* "Watch OS" (centre of the ring) */
lv_obj_t *ui_boot_version = NULL;   /* "v 1.0.0" */

/* ── Ring state ─────────────────────────────────────────────────────── */

static lv_obj_t  *s_ring        = NULL;  /* shared radial progress ring  */
static lv_timer_t *g_boot_timer = NULL;

static int      g_progress    = 0;       /* 0..RAMP_MAX, monotonic */
static uint16_t s_display_pct = 0;       /* last drawn overall percent */

/* Push the current progress to the shared ring (redraw only on change). */
static void ring_apply_progress(void)
{
    if (!s_ring) return;
    int pct = (g_progress * 100) / RAMP_MAX;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if ((uint16_t)pct != s_display_pct) {
        s_display_pct = (uint16_t)pct;
        ui_progress_ring_set_progress(s_ring, s_display_pct);
    }
}

/* ── Animation timer callback ───────────────────────────────────────── */

static void boot_anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_progress < RAMP_MAX) {
        g_progress += RAMP_DELTA;
        if (g_progress > RAMP_MAX) g_progress = RAMP_MAX;
    }
    ring_apply_progress();
}

/* ── Screen init ────────────────────────────────────────────────────── */

void ui_boot_up_screen_init(void)
{
    /* Already created? */
    if (ui_boot_up && lv_obj_is_valid(ui_boot_up)) return;

    g_progress    = 0;
    s_display_pct = 0;

    ui_boot_up = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_boot_up, LV_OBJ_FLAG_SCROLLABLE);

    /* Solid black background — no white flash */
    lv_obj_set_style_bg_color(ui_boot_up, lv_color_hex(0x000000),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_boot_up, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Radial-bar progress ring (behind the title, meets the edge) ── */
    s_ring = ui_progress_ring_create(ui_boot_up, RING_SIZE, RING_BARS);
    if (s_ring) {
        ui_progress_ring_set_bar_width(s_ring, RING_BAR_WIDTH);
        lv_obj_center(s_ring);
    }

    /* ── "Watch OS" title — exact centre of the screen ──────────────── */
    ui_boot_label = lv_label_create(ui_boot_up);
    lv_obj_set_width(ui_boot_label, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_boot_label, LV_SIZE_CONTENT);
    lv_obj_align(ui_boot_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(ui_boot_label, "Watch OS");
    lv_obj_set_style_text_color(ui_boot_label, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_boot_label, LV_OPA_COVER,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_boot_label, &lv_font_montserrat_48,
                               LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Version label ──────────────────────────────────────────────── */
    ui_boot_version = lv_label_create(ui_boot_up);
    lv_obj_set_width(ui_boot_version, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_boot_version, LV_SIZE_CONTENT);
    lv_obj_align(ui_boot_version, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_text(ui_boot_version, "v 1.0.0");
    lv_obj_set_style_text_color(ui_boot_version, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_boot_version, LV_OPA_COVER,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_boot_version, &lv_font_montserrat_16,
                               LV_PART_MAIN | LV_STATE_DEFAULT);

    ESP_LOGI(TAG, "Boot-up screen created");
}

/* ── Screen destroy ─────────────────────────────────────────────────── */

void ui_boot_up_screen_destroy(void)
{
    ui_boot_up_stop_animation();

    if (ui_boot_up && lv_obj_is_valid(ui_boot_up)) {
        /* lv_obj_del_async defers actual deletion to the end of the
         * current lv_timer_handler cycle — after all rendering. */
        lv_obj_del_async(ui_boot_up);
    }

    ui_boot_up      = NULL;
    ui_boot_label   = NULL;
    ui_boot_version = NULL;
    s_ring          = NULL;

    ESP_LOGI(TAG, "Boot-up screen destroyed");
}

/* ── Animation control ──────────────────────────────────────────────── */

void ui_boot_up_start_animation(void)
{
    if (g_boot_timer) return;  /* already running */

    g_progress    = 0;
    s_display_pct = 0;
    if (s_ring) ui_progress_ring_set_progress(s_ring, 0);

    /* 20 fps — smooth enough for OLED, light on CPU. */
    g_boot_timer = lv_timer_create(boot_anim_timer_cb, ANIM_PERIOD_MS, NULL);
    ESP_LOGI(TAG, "Boot animation started");
}

void ui_boot_up_stop_animation(void)
{
    if (g_boot_timer) {
        lv_timer_delete(g_boot_timer);
        g_boot_timer = NULL;
        ESP_LOGI(TAG, "Boot animation stopped");
    }
}

/* ── Progress control ───────────────────────────────────────────────── */

void ui_boot_up_bump_progress(int pct)
{
    if (!ui_boot_up || !lv_obj_is_valid(ui_boot_up)) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int target = pct * (RAMP_MAX / 100);
    if (target > g_progress) {
        g_progress = target;   /* ramp timer applies it on the next tick */
    }
}

void ui_boot_up_complete(void)
{
    if (!ui_boot_up || !lv_obj_is_valid(ui_boot_up)) return;
    g_progress = RAMP_MAX;
    ring_apply_progress();     /* light every remaining bar right now */
}

