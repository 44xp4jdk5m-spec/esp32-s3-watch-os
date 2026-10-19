/**
 * @file photos_ui.c
 * @brief Photos app — multi-format viewer with streaming gallery (PSRAM)
 *
 * Decoders:
 *   - BMP  → LV_USE_BMP
 *   - JPG  → LV_USE_TJPGD
 *   - PNG  → LV_USE_LODEPNG
 *   - GIF  → LV_USE_GIF
 *   - WEBP → webp_decoder_init() (custom embedded decoder)
 *
 * Gallery:
 *   - 3-column grid with streaming window: only visible thumbnails are
 *     decoded; images outside the viewport + 1-row buffer are freed.
 *   - All large allocations use PSRAM (MALLOC_CAP_SPIRAM).
 *
 * Home screen:
 *   - Horizontal scroll-snap container with two panels.
 *   - Panel 0 shows the chronologically last image from SD card.
 *   - 2-second slideshow fades backward through the file list.
 */

#include "photos_ui.h"
#include "photo_cache.h"
#include "webp_decoder.h"
#include "hw_display.h"
#include "hw_rtc.h"
#include "app_events.h"
#include "app_config.h"
#include "app_lang.h"
#include "fonts.h"
#include "noto_font_24_bold.h"
#include "mi_symbols.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "PHOTOS";

/* ==================================================================== */
/*  Theme constants                                                     */
/* ==================================================================== */
#define EOS_COLOR_WHITE       lv_color_hex(0xFFFFFF)
#define EOS_COLOR_BLACK       lv_color_hex(0x000000)
#define EOS_COLOR_DARK_GREY_2 lv_color_hex(0x242424)
#define EOS_COLOR_GREY        lv_color_hex(0x9BA0AA)
#define EOS_COLOR_YELLOW      lv_color_hex(0xFFD60A)

#define EOS_DISPLAY_WIDTH     410
#define EOS_DISPLAY_HEIGHT    502

#define _HEADER_HEIGHT        120
#define _HEADER_MARGIN_RIGHT  30
#define _HEADER_TITLE_WIDTH   240
#define _TITLE_LABEL_Y_OFFSET 20
#define _TITLE_LABEL_X_OFFSET (-_HEADER_MARGIN_RIGHT)
#define _BACK_BTN_MARGIN_LEFT 20
#define _BACK_BTN_WIDTH       64
#define _BACK_BTN_HEIGHT      64

/* SD card limits */
#define PHOTOS_MAX_FILES      512
#define PHOTOS_FILENAME_MAX   128

/* ==================================================================== */
/*  Supported image extensions                                          */
/* ==================================================================== */
static const char *s_extensions[] = {
    ".bmp", ".jpg", ".jpeg", ".png", ".gif", ".webp"
};
#define EXT_COUNT (sizeof(s_extensions) / sizeof(s_extensions[0]))

static bool _has_image_ext(const char *name)
{
    size_t len = strlen(name);
    for (int i = 0; i < (int)EXT_COUNT; i++) {
        size_t elen = strlen(s_extensions[i]);
        if (len < elen) continue;
        const char *end = name + len - elen;
        bool match = true;
        for (size_t j = 0; j < elen; j++) {
            if (tolower((unsigned char)end[j]) !=
                tolower((unsigned char)s_extensions[i][j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/* ==================================================================== */
/*  Gallery slot — one cell + optional cached thumbnail                 */
/* ==================================================================== */
typedef struct {
    lv_obj_t      *cell;        /* Container (always exists) */
    lv_obj_t      *img;         /* Image object (always exists) */
    int            file_idx;    /* Index into file_list, -1 = placeholder */
    lv_draw_buf_t *cache_db;    /* Cached thumbnail draw_buf (PSRAM), NULL = not loaded */
    lv_image_dsc_t cache_dsc;   /* Image descriptor wrapping cache_db pixel data */
} gallery_slot_t;

/* ==================================================================== */
/*  Global context                                                      */
/* ==================================================================== */
typedef struct {
    lv_obj_t      *scr;              /* current top-level screen */
    lv_timer_t    *clock_timer;
    lv_timer_t    *slideshow_timer;  /* 2s fade for panel 0 */

    /* Home screen */
    lv_obj_t      *container;        /* horizontal scroll container */
    lv_obj_t      *panel_index_0;
    lv_obj_t      *panel_index_1;
    lv_obj_t      *image0;           /* image in panel 0 */
    lv_obj_t      *dot_panel;
    lv_obj_t      *index0_dot;
    lv_obj_t      *index1_dot;
    lv_obj_t      *photo_count_0;
    lv_obj_t      *location0;

    /* Gallery screen */
    lv_obj_t      *gallery_scr;
    lv_obj_t      *gallery_grid;
    lv_obj_t      *hdr, *hdr_clock, *hdr_title;

    /* Streaming gallery slots */
    gallery_slot_t *slots;
    int             slot_count;

    /* Image file list */
    char           file_list[PHOTOS_MAX_FILES][PHOTOS_FILENAME_MAX];
    int            file_count;
    int            current_index;    /* slideshow index */

    /* State */
    bool           active;
    bool           on_gallery;
} ctx_t;

static ctx_t *G;
static lv_obj_t *s_prev_screen;

/* Forward declarations */
static void _go_home(void);
static void _go_gallery(void);
static void _build_home_screen(void);
static void _build_gallery_screen(void);
static void _gallery_scroll_cb(lv_event_t *e);

/* ==================================================================== */
/*  SD card file scanning                                               */
/* ==================================================================== */
static int _scan_image_files(void)
{
    int count = 0;
    DIR *dir = opendir("/sdcard/shots");
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open /sdcard/shots");
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < PHOTOS_MAX_FILES) {
        if (entry->d_type == DT_REG && _has_image_ext(entry->d_name)) {
            snprintf(G->file_list[count], PHOTOS_FILENAME_MAX,
                     "S:/shots/%s", entry->d_name);
            count++;
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "Found %d images (BMP/JPG/PNG/GIF/WEBP)", count);

    /* Sort by filename */
    if (count > 1) {
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - 1 - i; j++) {
                if (strcmp(G->file_list[j], G->file_list[j + 1]) > 0) {
                    char tmp[PHOTOS_FILENAME_MAX];
                    strcpy(tmp, G->file_list[j]);
                    strcpy(G->file_list[j], G->file_list[j + 1]);
                    strcpy(G->file_list[j + 1], tmp);
                }
            }
        }
    }

    return count;
}

/* ==================================================================== */
/*  Clock helper                                                        */
/* ==================================================================== */
static void _clock_update_cb(lv_timer_t *t)
{
    lv_obj_t *label = lv_timer_get_user_data(t);
    if (!G || !label || !lv_obj_is_valid(label)) return;

    hw_rtc_time_t rt;
    if (hw_rtc_get_time(&rt) != ESP_OK) return;

    time_t ts = hw_rtc_to_unix(&rt) + (g_config.timezone_offset * 3600);
    hw_rtc_time_t local;
    hw_rtc_from_unix(ts, &local);

    lv_label_set_text_fmt(label, "%02d:%02d", local.hour, local.min);

    uint32_t next_ms = (uint32_t)(60 - rt.sec) * 1000;
    if (next_ms < 1000) next_ms = 60000;
    lv_timer_set_period(t, next_ms);
}

/* ==================================================================== */
/*  Slideshow — fade to previous image every 2 seconds                  */
/* ==================================================================== */
static void _slideshow_cb(lv_timer_t *t)
{
    (void)t;
    if (!G || !G->active || G->file_count == 0 ||
        !G->image0 || !lv_obj_is_valid(G->image0))
        return;

    G->current_index--;
    if (G->current_index < 0)
        G->current_index = G->file_count - 1;

    lv_obj_set_style_opa(G->image0, LV_OPA_TRANSP, 0);
    lv_image_set_src(G->image0, G->file_list[G->current_index]);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, G->image0);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 400);
    lv_anim_start(&a);
}

/* ==================================================================== */
/*  Back button callback                                                */
/* ==================================================================== */
static void _back_btn_cb(lv_event_t *e)
{
    (void)e;
    photos_ui_handle_back();
}

/* ==================================================================== */
/*  Panel 0 click → gallery                                             */
/* ==================================================================== */
static void _panel0_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED)
        _go_gallery();
}

/* ==================================================================== */
/*  Scroll indicator update for home screen                             */
/* ==================================================================== */
static void _scroll_update_cb(lv_event_t *e)
{
    if (!G || !G->active) return;
    lv_obj_t *cont = lv_event_get_target(e);
    int32_t scroll_x = lv_obj_get_scroll_x(cont);
    int32_t w = lv_obj_get_width(cont);
    bool panel0_active = (scroll_x < w / 2);

    if (G->index0_dot && lv_obj_is_valid(G->index0_dot)) {
        lv_obj_set_style_bg_color(G->index0_dot,
            panel0_active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x9BA0AA), 0);
    }
    if (G->index1_dot && lv_obj_is_valid(G->index1_dot)) {
        lv_obj_set_style_bg_color(G->index1_dot,
            panel0_active ? lv_color_hex(0x9BA0AA) : lv_color_hex(0xFFFFFF), 0);
    }
}

/* ==================================================================== */
/*  Header builder                                                      */
/* ==================================================================== */
static void _build_header(const char *title)
{
    if (G->clock_timer) {
        lv_timer_del(G->clock_timer);
        G->clock_timer = NULL;
    }
    if (G->hdr) { lv_obj_del(G->hdr); }
    G->hdr = G->hdr_clock = G->hdr_title = NULL;

    G->hdr = lv_obj_create(G->scr);
    lv_obj_set_size(G->hdr, EOS_DISPLAY_WIDTH, _HEADER_HEIGHT);
    lv_obj_align(G->hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(G->hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(G->hdr, 0, 0);
    lv_obj_set_style_pad_all(G->hdr, 0, 0);
    lv_obj_set_style_radius(G->hdr, 0, 0);
    lv_obj_remove_flag(G->hdr, LV_OBJ_FLAG_SCROLLABLE);

    /* Header fade mask */
    lv_obj_t *mask = lv_obj_create(G->hdr);
    lv_obj_set_size(mask, EOS_DISPLAY_WIDTH, 125);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(mask, EOS_COLOR_BLACK, 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(mask, EOS_COLOR_WHITE, 0);
    lv_obj_set_style_bg_grad_dir(mask, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(mask, 175, 0);
    lv_obj_set_style_bg_grad_stop(mask, 255, 0);
    lv_obj_set_style_blend_mode(mask, LV_BLEND_MODE_MULTIPLY, 0);
    lv_obj_set_style_opa(mask, LV_OPA_COVER, 0);

    /* Back button */
    lv_obj_t *bb = lv_button_create(G->hdr);
    lv_obj_set_size(bb, _BACK_BTN_WIDTH, _BACK_BTN_HEIGHT);
    lv_obj_set_style_radius(bb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bb, EOS_COLOR_DARK_GREY_2, 0);
    lv_obj_set_style_border_width(bb, 0, 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_align(bb, LV_ALIGN_LEFT_MID, _BACK_BTN_MARGIN_LEFT + 6, 4);
    lv_obj_add_event_cb(bb, _back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb);
    lv_label_set_text(bl, MI_ARROW_BACK);
    lv_obj_set_style_text_font(bl, &mi_font_24, 0);
    lv_obj_set_style_text_color(bl, EOS_COLOR_YELLOW, 0);
    lv_obj_align(bl, LV_ALIGN_CENTER, -2, 2);
    lv_obj_set_style_transform_pivot_x(bb, 32, 0);
    lv_obj_set_style_transform_pivot_y(bb, 32, 0);

    /* Clock */
    G->hdr_clock = lv_label_create(G->hdr);
    lv_obj_set_style_text_font(G->hdr_clock, &noto_font_24_bold, 0);
    lv_obj_set_style_text_color(G->hdr_clock, EOS_COLOR_WHITE, 0);
    lv_obj_align(G->hdr_clock, LV_ALIGN_RIGHT_MID, -_HEADER_MARGIN_RIGHT, -14);

    G->clock_timer = lv_timer_create(_clock_update_cb, 100, G->hdr_clock);
    lv_timer_set_repeat_count(G->clock_timer, -1);
    _clock_update_cb(G->clock_timer);

    /* Title */
    G->hdr_title = lv_label_create(G->hdr);
    lv_obj_set_width(G->hdr_title, _HEADER_TITLE_WIDTH);
    lv_obj_set_style_text_font(G->hdr_title, &noto_font_24, 0);
    lv_label_set_long_mode(G->hdr_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(G->hdr_title, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(G->hdr_title, EOS_COLOR_YELLOW, 0);
    lv_obj_align(G->hdr_title, LV_ALIGN_RIGHT_MID,
                 _TITLE_LABEL_X_OFFSET, _TITLE_LABEL_Y_OFFSET);
    lv_label_set_text(G->hdr_title, title);
}

/* ==================================================================== */
/*  Home screen                                                         */
/* ==================================================================== */
static void _build_home_screen(void)
{
    if (!G || !G->scr) return;

    /* "Photos" title, yellow, top-left */
    lv_obj_t *title = lv_label_create(G->scr);
    lv_obj_set_width(title, LV_SIZE_CONTENT);
    lv_obj_set_x(title, 35);
    lv_obj_set_y(title, 70);
    lv_label_set_text(title, app_lang_get("photos.title"));
    lv_obj_set_style_text_color(title, EOS_COLOR_YELLOW, 0);
    lv_obj_set_style_text_font(title, &noto_font_48, 0);

    /* Time label */
    lv_obj_t *tl = lv_label_create(G->scr);
    lv_obj_set_style_text_font(tl, &noto_font_24, 0);
    lv_obj_set_style_text_color(tl, EOS_COLOR_WHITE, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_RIGHT, -_HEADER_MARGIN_RIGHT, 46);

    lv_timer_t *t = lv_timer_create(_clock_update_cb, 100, tl);
    lv_timer_set_repeat_count(t, -1);
    _clock_update_cb(t);
    if (G->clock_timer) lv_timer_del(G->clock_timer);
    G->clock_timer = t;

    /* Dot indicator panel */
    G->dot_panel = lv_obj_create(G->scr);
    lv_obj_set_size(G->dot_panel, 34, 18);
    lv_obj_set_x(G->dot_panel, 0);
    lv_obj_set_y(G->dot_panel, 381);
    lv_obj_set_align(G->dot_panel, LV_ALIGN_TOP_MID);
    lv_obj_remove_flag(G->dot_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(G->dot_panel, 50, 0);
    lv_obj_set_style_bg_color(G->dot_panel, lv_color_hex(0x242424), 0);
    lv_obj_set_style_bg_opa(G->dot_panel, 255, 0);

    G->index0_dot = lv_obj_create(G->dot_panel);
    lv_obj_set_size(G->index0_dot, 10, 10);
    lv_obj_set_x(G->index0_dot, 5);
    lv_obj_set_y(G->index0_dot, 0);
    lv_obj_set_align(G->index0_dot, LV_ALIGN_LEFT_MID);
    lv_obj_remove_flag(G->index0_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(G->index0_dot, 50, 0);
    lv_obj_set_style_bg_color(G->index0_dot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(G->index0_dot, 255, 0);

    G->index1_dot = lv_obj_create(G->dot_panel);
    lv_obj_set_size(G->index1_dot, 10, 10);
    lv_obj_set_x(G->index1_dot, -5);
    lv_obj_set_y(G->index1_dot, 0);
    lv_obj_set_align(G->index1_dot, LV_ALIGN_RIGHT_MID);
    lv_obj_remove_flag(G->index1_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(G->index1_dot, 50, 0);
    lv_obj_set_style_bg_color(G->index1_dot, lv_color_hex(0x9BA0AA), 0);
    lv_obj_set_style_bg_opa(G->index1_dot, 255, 0);

    /* Scroll container */
    G->container = lv_obj_create(G->scr);
    lv_obj_remove_style_all(G->container);
    lv_obj_set_size(G->container, 410, 250);
    lv_obj_set_x(G->container, 0);
    lv_obj_set_y(G->container, 128);
    lv_obj_remove_flag(G->container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(G->container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(G->container, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(G->container, LV_SCROLL_SNAP_CENTER);

    /* Panel 0 */
    G->panel_index_0 = lv_obj_create(G->container);
    lv_obj_set_size(G->panel_index_0, 350, 250);
    lv_obj_set_x(G->panel_index_0, 30);
    lv_obj_set_align(G->panel_index_0, LV_ALIGN_LEFT_MID);
    lv_obj_remove_flag(G->panel_index_0, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(G->panel_index_0, 30, 0);
    lv_obj_set_style_bg_color(G->panel_index_0, lv_color_hex(0x242424), 0);
    lv_obj_set_style_bg_opa(G->panel_index_0, 255, 0);
    lv_obj_set_style_clip_corner(G->panel_index_0, true, 0);

    G->image0 = lv_image_create(G->panel_index_0);
    lv_obj_set_size(G->image0, 350, 250);
    lv_obj_set_align(G->image0, LV_ALIGN_CENTER);
    lv_obj_remove_flag(G->image0, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_inner_align(G->image0, LV_IMAGE_ALIGN_CONTAIN);

    if (G->file_count > 0) {
        G->current_index = G->file_count - 1;
        lv_image_set_src(G->image0, G->file_list[G->current_index]);
    }

    /* Fade mask panel 0 */
    lv_obj_t *fm0 = lv_obj_create(G->panel_index_0);
    lv_obj_set_size(fm0, 350, 83);
    lv_obj_set_align(fm0, LV_ALIGN_BOTTOM_MID);
    lv_obj_remove_flag(fm0, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(fm0, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(fm0, 255, 0);
    lv_obj_set_style_bg_grad_color(fm0, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(fm0, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_blend_mode(fm0, LV_BLEND_MODE_MULTIPLY, 0);
    if (G->file_count == 0) lv_obj_add_flag(fm0, LV_OBJ_FLAG_HIDDEN);

    G->location0 = lv_label_create(G->panel_index_0);
    lv_obj_set_style_text_color(G->location0, EOS_COLOR_WHITE, 0);
    lv_obj_set_style_text_font(G->location0, &noto_font_24, 0);
    lv_obj_align(G->location0, LV_ALIGN_BOTTOM_LEFT, 12, -36);
    lv_label_set_text(G->location0, app_lang_get("photos.watch"));

    G->photo_count_0 = lv_label_create(G->panel_index_0);
    lv_obj_set_style_text_color(G->photo_count_0, lv_color_hex(0x9BA0AA), 0);
    lv_obj_set_style_text_font(G->photo_count_0, &noto_font_14, 0);
    lv_obj_align(G->photo_count_0, LV_ALIGN_BOTTOM_LEFT, 12, -14);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d %s", G->file_count, app_lang_get("photos.count_word"));
    lv_label_set_text(G->photo_count_0, buf);

    lv_obj_add_flag(G->panel_index_0, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(G->panel_index_0, _panel0_click_cb,
                        LV_EVENT_SHORT_CLICKED, NULL);

    /* Panel 1 (placeholder) */
    G->panel_index_1 = lv_obj_create(G->container);
    lv_obj_set_size(G->panel_index_1, 350, 250);
    lv_obj_set_x(G->panel_index_1, 380);
    lv_obj_set_align(G->panel_index_1, LV_ALIGN_RIGHT_MID);
    lv_obj_remove_flag(G->panel_index_1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(G->panel_index_1, 30, 0);
    lv_obj_set_style_bg_color(G->panel_index_1, lv_color_hex(0x242424), 0);
    lv_obj_set_style_clip_corner(G->panel_index_1, true, 0);

    lv_obj_t *im1 = lv_image_create(G->panel_index_1);
    lv_obj_set_align(im1, LV_ALIGN_CENTER);
    lv_obj_remove_flag(im1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fm1 = lv_obj_create(G->panel_index_1);
    lv_obj_set_size(fm1, 350, 83);
    lv_obj_set_align(fm1, LV_ALIGN_BOTTOM_MID);
    lv_obj_remove_flag(fm1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(fm1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(fm1, 255, 0);
    lv_obj_set_style_bg_grad_color(fm1, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(fm1, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_blend_mode(fm1, LV_BLEND_MODE_MULTIPLY, 0);
    if (G->file_count == 0) lv_obj_add_flag(fm1, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *loc1 = lv_label_create(G->panel_index_1);
    lv_obj_set_style_text_color(loc1, EOS_COLOR_WHITE, 0);
    lv_obj_set_style_text_font(loc1, &noto_font_24, 0);
    lv_obj_align(loc1, LV_ALIGN_BOTTOM_LEFT, 12, -36);
    lv_label_set_text(loc1, app_lang_get("photos.google_watch"));

    lv_obj_t *pc1 = lv_label_create(G->panel_index_1);
    lv_obj_set_style_text_color(pc1, lv_color_hex(0x9BA0AA), 0);
    lv_obj_set_style_text_font(pc1, &noto_font_14, 0);
    lv_obj_align(pc1, LV_ALIGN_BOTTOM_LEFT, 12, -14);
    lv_label_set_text(pc1, app_lang_get("photos.no_photos"));

    /* Scroll indicator update */
    lv_obj_add_event_cb(G->container, _scroll_update_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(G->container, _scroll_update_cb, LV_EVENT_SCROLL_END, NULL);

    /* Slideshow */
    if (G->file_count > 1 && !G->slideshow_timer) {
        G->slideshow_timer = lv_timer_create(_slideshow_cb, 2000, NULL);
        lv_timer_set_repeat_count(G->slideshow_timer, -1);
    }
}

/* ==================================================================== */
/*  Streaming gallery — decode only visible thumbnails via SD card cache */
/* ==================================================================== */

/* Cell size: 136px (410/3 rounded) */
#define CELL_SIZE  136
#define COLS        3
#define STREAM_BUF  1  /* extra rows above/below viewport */

/**
 * @brief Ensure a slot has its thumbnail loaded into PSRAM.
 *
 * First tries the SD card cache (fast).  If no cache exists, decodes
 * the original image, resizes, and writes to cache (slow once).
 *
 * @return true if thumbnail is now available
 */
static bool _slot_load_thumbnail(gallery_slot_t *slot)
{
    if (!slot || slot->file_idx < 0 || slot->file_idx >= G->file_count)
        return false;

    /* Already loaded */
    if (slot->cache_db) return true;

    const char *path = G->file_list[slot->file_idx];

    /* Try loading from cache first */
    lv_draw_buf_t *db = photo_cache_load(path);
    if (!db) {
        /* Generate cache (decode + resize + save to SD card) */
        ESP_LOGI(TAG, "Generating cache for: %s", path);
        db = photo_cache_generate(path, CELL_SIZE, CELL_SIZE);
    }

    if (!db) return false;

    /* Store and set as image source */
    slot->cache_db = db;

    /* Build image descriptor from draw_buf */
    memset(&slot->cache_dsc, 0, sizeof(slot->cache_dsc));
    slot->cache_dsc.header.magic     = LV_IMAGE_HEADER_MAGIC;
    slot->cache_dsc.header.cf        = db->header.cf;
    slot->cache_dsc.header.w         = db->header.w;
    slot->cache_dsc.header.h         = db->header.h;
    slot->cache_dsc.header.stride    = db->header.stride;
    slot->cache_dsc.data_size        = db->data_size;
    slot->cache_dsc.data             = db->data;

    lv_image_set_src(slot->img, &slot->cache_dsc);
    return true;
}

/**
 * @brief Free a slot's cached thumbnail data.
 */
static void _slot_unload_thumbnail(gallery_slot_t *slot)
{
    if (!slot || !slot->cache_db) return;

    lv_image_set_src(slot->img, NULL);
    lv_draw_buf_destroy(slot->cache_db);
    slot->cache_db = NULL;
    memset(&slot->cache_dsc, 0, sizeof(slot->cache_dsc));
}

static void _gallery_scroll_cb(lv_event_t *e)
{
    (void)e;
    if (!G || !G->active || !G->on_gallery || !G->gallery_grid || !G->slots)
        return;

    lv_obj_t *grid = G->gallery_grid;
    int32_t scroll_y = lv_obj_get_scroll_y(grid);
    int32_t grid_h  = lv_obj_get_height(grid);
    int32_t vis_top = scroll_y;
    int32_t vis_bot = scroll_y + grid_h;

    /* Visible range + buffer */
    int vis_row_first = (vis_top / CELL_SIZE) - STREAM_BUF;
    int vis_row_last  = (vis_bot / CELL_SIZE) + STREAM_BUF;
    if (vis_row_first < 0) vis_row_first = 0;

    int total_rows = (G->slot_count + COLS - 1) / COLS;
    if (vis_row_last >= total_rows) vis_row_last = total_rows - 1;

    for (int i = 0; i < G->slot_count; i++) {
        gallery_slot_t *slot = &G->slots[i];
        if (!slot->cell || !lv_obj_is_valid(slot->cell)) continue;
        if (slot->file_idx < 0) continue;

        int row = i / COLS;
        bool in_window = (row >= vis_row_first && row <= vis_row_last);

        if (in_window) {
            /* Load or ensure thumbnail is in PSRAM */
            _slot_load_thumbnail(slot);
        } else {
            /* Outside viewport — free PSRAM */
            _slot_unload_thumbnail(slot);
        }
    }
}

static void _build_gallery_screen(void)
{
    if (!G) return;

    G->gallery_scr = lv_obj_create(NULL);
    lv_obj_set_size(G->gallery_scr, EOS_DISPLAY_WIDTH, EOS_DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(G->gallery_scr, EOS_COLOR_BLACK, 0);
    lv_obj_set_style_bg_opa(G->gallery_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(G->gallery_scr, 0, 0);
    lv_obj_set_style_pad_all(G->gallery_scr, 0, 0);
    lv_obj_remove_flag(G->gallery_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Build header */
    G->hdr = NULL;
    G->hdr_clock = NULL;
    G->hdr_title = NULL;
    lv_obj_t *saved_scr = G->scr;
    G->scr = G->gallery_scr;
    _build_header(app_lang_get("photos.title"));
    G->scr = saved_scr;

    /* Grid container */
    G->gallery_grid = lv_obj_create(G->gallery_scr);
    lv_obj_remove_style_all(G->gallery_grid);
    lv_obj_set_size(G->gallery_grid, EOS_DISPLAY_WIDTH, EOS_DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(G->gallery_grid, EOS_COLOR_BLACK, 0);
    lv_obj_set_style_bg_opa(G->gallery_grid, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(G->gallery_grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(G->gallery_grid, LV_DIR_VER);
    lv_obj_set_style_pad_top(G->gallery_grid, _HEADER_HEIGHT + 10, 0);
    lv_obj_set_style_pad_bottom(G->gallery_grid, 40, 0);
    lv_obj_set_style_pad_left(G->gallery_grid, 0, 0);
    lv_obj_set_style_pad_right(G->gallery_grid, 0, 0);
    lv_obj_set_flex_flow(G->gallery_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(G->gallery_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (G->file_count == 0) {
        /* No photos label */
        lv_obj_t *nl = lv_label_create(G->gallery_grid);
        lv_obj_set_width(nl, EOS_DISPLAY_WIDTH);
        lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(nl, app_lang_get("photos.no_photos"));
        lv_obj_set_style_text_color(nl, EOS_COLOR_GREY, 0);
        lv_obj_set_style_text_font(nl, &noto_font_24, 0);
        lv_obj_set_y(nl, 200);
        G->slots = NULL;
        G->slot_count = 0;
    } else {
        /* Allocate slots */
        G->slot_count = G->file_count;
        G->slots = (gallery_slot_t *)heap_caps_calloc(
            G->slot_count, sizeof(gallery_slot_t), MALLOC_CAP_SPIRAM);
        if (!G->slots) {
            ESP_LOGE(TAG, "PSRAM alloc failed for gallery slots");
            G->slot_count = 0;
        } else {
            /* Create all cell containers (cheap — no decoded data yet) */
            for (int i = 0; i < G->slot_count; i++) {
                gallery_slot_t *slot = &G->slots[i];
                slot->file_idx = i;

                slot->cell = lv_obj_create(G->gallery_grid);
                lv_obj_set_size(slot->cell, CELL_SIZE, CELL_SIZE);
                lv_obj_set_style_bg_color(slot->cell,
                    lv_color_hex(0x1A1A1A), 0);
                lv_obj_set_style_bg_opa(slot->cell, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(slot->cell, 0, 0);
                lv_obj_set_style_pad_all(slot->cell, 0, 0);
                lv_obj_set_style_radius(slot->cell, 0, 0);
                lv_obj_remove_flag(slot->cell, LV_OBJ_FLAG_SCROLLABLE);

                slot->img = lv_image_create(slot->cell);
                lv_obj_set_size(slot->img, CELL_SIZE, CELL_SIZE);
                lv_obj_set_align(slot->img, LV_ALIGN_CENTER);
                lv_image_set_inner_align(slot->img, LV_IMAGE_ALIGN_CONTAIN);
                /* No src set yet — scroll callback will load cached thumbnails */
            }

            /* Register scroll callback for streaming */
            lv_obj_add_event_cb(G->gallery_grid, _gallery_scroll_cb,
                                LV_EVENT_SCROLL, NULL);
            lv_obj_add_event_cb(G->gallery_grid, _gallery_scroll_cb,
                                LV_EVENT_SCROLL_END, NULL);

            /* Also load on screen-ready (belt AND suspenders) */
            lv_obj_add_event_cb(G->gallery_scr, _gallery_scroll_cb,
                                LV_EVENT_SCREEN_LOADED, NULL);
        }
    }

    /* Move header to front */
    if (G->hdr) lv_obj_move_foreground(G->hdr);
}

/* ==================================================================== */
/*  Screen transitions                                                  */
/* ==================================================================== */
static void _go_gallery(void)
{
    if (!G || !G->active) return;

    if (G->slideshow_timer) {
        lv_timer_del(G->slideshow_timer);
        G->slideshow_timer = NULL;
    }
    if (G->clock_timer) {
        lv_timer_del(G->clock_timer);
        G->clock_timer = NULL;
    }

    _build_gallery_screen();
    lv_screen_load_anim(G->gallery_scr, LV_SCR_LOAD_ANIM_OVER_LEFT, 280, 0, true);
    G->on_gallery = true;
}

static void _go_home(void)
{
    if (!G || !G->active) return;

    if (G->clock_timer) {
        lv_timer_del(G->clock_timer);
        G->clock_timer = NULL;
    }

    /* Free gallery slots */
    if (G->slots) {
        /* Clear cached thumbnails */
        for (int i = 0; i < G->slot_count; i++) {
            _slot_unload_thumbnail(&G->slots[i]);
        }
        heap_caps_free(G->slots);
        G->slots = NULL;
        G->slot_count = 0;
    }

    lv_obj_t *new_scr = lv_obj_create(NULL);
    lv_obj_set_size(new_scr, EOS_DISPLAY_WIDTH, EOS_DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(new_scr, EOS_COLOR_BLACK, 0);
    lv_obj_set_style_bg_opa(new_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(new_scr, 0, 0);
    lv_obj_set_style_pad_all(new_scr, 0, 0);
    lv_obj_remove_flag(new_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Reset all context pointers */
    G->container = G->panel_index_0 = G->panel_index_1 = NULL;
    G->image0 = G->dot_panel = NULL;
    G->index0_dot = G->index1_dot = NULL;
    G->photo_count_0 = G->location0 = NULL;
    G->hdr = G->hdr_clock = G->hdr_title = NULL;
    G->gallery_scr = G->gallery_grid = NULL;

    G->scr = new_scr;
    _build_home_screen();
    lv_screen_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 280, 0, true);
    G->on_gallery = false;
}

/* ==================================================================== */
/*  Back navigation                                                     */
/* ==================================================================== */
void photos_ui_handle_back(void)
{
    if (!G || !G->active) return;

    if (G->on_gallery) {
        ESP_LOGI(TAG, "Back from gallery → home");
        _go_home();
    } else {
        ESP_LOGI(TAG, "Back at home → exit photos");
        photos_ui_destroy();
    }
}

/* ==================================================================== */
/*  Public API                                                          */
/* ==================================================================== */
void photos_ui_create(void)
{
    if (G) { ESP_LOGW(TAG, "Already active"); return; }

    /* Register WEBP decoder once */
    webp_decoder_init();

    G = (ctx_t *)heap_caps_calloc(1, sizeof(ctx_t), MALLOC_CAP_SPIRAM);
    if (!G) {
        G = calloc(1, sizeof(ctx_t));
        if (!G) return;
    }

    s_prev_screen = lv_screen_active();

    G->file_count = _scan_image_files();

    G->scr = lv_obj_create(NULL);
    lv_obj_set_size(G->scr, EOS_DISPLAY_WIDTH, EOS_DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(G->scr, EOS_COLOR_BLACK, 0);
    lv_obj_set_style_bg_opa(G->scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(G->scr, 0, 0);
    lv_obj_set_style_pad_all(G->scr, 0, 0);
    lv_obj_remove_flag(G->scr, LV_OBJ_FLAG_SCROLLABLE);
    G->active = true;

    _build_home_screen();
    lv_screen_load(G->scr);
    G->on_gallery = false;
    ESP_LOGI(TAG, "Created (%d images)", G->file_count);
}

void photos_ui_destroy(void)
{
    if (!G) return;
    G->active = false;

    if (G->clock_timer) { lv_timer_del(G->clock_timer); G->clock_timer = NULL; }
    if (G->slideshow_timer) { lv_timer_del(G->slideshow_timer); G->slideshow_timer = NULL; }

    /* Free gallery slots */
    if (G->slots) {
        for (int i = 0; i < G->slot_count; i++) {
            _slot_unload_thumbnail(&G->slots[i]);
        }
        heap_caps_free(G->slots);
        G->slots = NULL;
        G->slot_count = 0;
    }

    if (s_prev_screen && lv_obj_is_valid(s_prev_screen))
        lv_screen_load_anim(s_prev_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, true);

    heap_caps_free(G);
    G = NULL;
    ESP_LOGI(TAG, "Destroyed");
}

bool photos_ui_is_active(void)
{
    return G && G->active;
}
