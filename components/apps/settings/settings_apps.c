/**
 * @file settings_apps.c
 * @brief Settings > Apps pages — auto-read app list + app info page.
 *
 * Pixel-matched to the ElenixOS reference (eos_settings.c +
 * eos_basic_widgets.c):
 *   - Apps list  = flat list of `eos_list_add_button` rows (64px circular
 *                  icon + name, margin 10, no chevron / tag).
 *   - App info   = a 24px-padded rounded panel (0x242424) holding a
 *                  100px circular icon + name header and label/value
 *                  rows (ID / Author / Version / SDK), an optional
 *                  Description section, then the "Clear data" and
 *                  "Uninstall" full-width red-text pills — mirroring
 *                  `_settings_app_list_btn_cb` exactly.
 */

#include "settings_ui_internal.h"
#include "sw_list.h"
#include "sw_row.h"
#include "sw_comment.h"
#include "sw_button.h"
#include "sw_icons.h"
#include "app_registry.h"
#include "js_app_registry.h"
#include "actionsheet.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SETTINGS";

/* Forward: app list row defined after the image helper. */
static lv_obj_t *_app_list_row(lv_obj_t *list, const lv_image_dsc_t *img,
                               const char *name, lv_event_cb_t cb, void *ud);

/* Which app the info page (P_APP_INFO) is showing. */
static int s_info_index = 0;

void settings_apps_set_info_index(int index) { s_info_index = index; }

const char *settings_apps_info_name(void)
{
    const app_entry_t *a = app_registry_get(s_info_index);
    return (a && a->name) ? a->name : "App";
}

/* ====================================================================
 * Apps list page (P_APPS)
 * ==================================================================== */
static void _app_row_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    settings_ui_vibe();
    settings_apps_set_info_index(idx);
    settings_ui_go(P_APP_INFO);
}

static void _build_apps_page(void)
{
    lv_obj_t *list = settings_ui_list();
    int total = app_registry_count();

    if (total == 0) {
        sw_comment_add(list, "No apps installed");
        return;
    }

    /* Flat list of eos_list_add_button-style rows (64px icon + name). */
    for (int i = 0; i < total; i++) {
        const app_entry_t *a = app_registry_get(i);
        if (!a) continue;
        _app_list_row(list, a->icon, a->name,
                      _app_row_cb, (void *)(uintptr_t)i);
    }
}

/* ====================================================================
 * App info page (P_APP_INFO)
 *
 * Mirrors the reference `_settings_app_list_btn_cb` layout.  The header/
 * info rows live in ONE rounded panel (`eos_list_add_container`) which
 * must use COLUMN flex — otherwise every row is stacked at the same spot
 * and the text overlaps (that was the bug).
 * ==================================================================== */

/* Circular-clipped image (matches eos_circle_image_create, COVER mode). */
static void _app_circle_image(lv_obj_t *parent, const lv_image_dsc_t *img,
                              lv_coord_t size)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    if (img && img->header.w > 0 && img->header.h > 0) {
        lv_obj_t *im = lv_image_create(c);
        lv_image_set_src(im, img);
        lv_image_set_inner_align(im, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_size(im, size, size);
        /* Cover: use the larger of the two scale factors. */
        uint32_t sx = ((uint32_t)size << 8) / img->header.w;
        uint32_t sy = ((uint32_t)size << 8) / img->header.h;
        uint32_t s  = (sx > sy) ? sx : sy;
        lv_image_set_scale_x(im, s);
        lv_image_set_scale_y(im, s);
        lv_obj_center(im);
    } else {
        lv_obj_t *icon = lv_label_create(c);
        lv_label_set_text(icon, SW_ICON_APPS);
        lv_obj_set_style_text_font(icon, SW_ICON_F, 0);
        lv_obj_set_style_text_color(icon, SW_COLOR_WHITE, 0);
        lv_obj_center(icon);
    }
}

/* App list row (special-cased in the Apps page — NOT a shared widget):
 * 64px circular icon + name, no chevron. */
static lv_obj_t *_app_list_row(lv_obj_t *list, const lv_image_dsc_t *img,
                               const char *name, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = sw_row_create(list);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_USER_1);
    lv_obj_set_style_pad_left(btn, EOS_WX(24), 0);
    _app_circle_image(btn, img, EOS_WX(64));

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, SW_FONT_M, 0);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_margin_left(label, EOS_WX(10), 0);
    lv_obj_set_style_text_color(label, SW_COLOR_WHITE, 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    return btn;
}

/* label/value row (matches eos_row_create: flex space-between). */
static void _app_info_row(lv_obj_t *parent, const char *label, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, SW_FONT_M, 0);
    lv_obj_set_style_text_color(l, SW_COLOR_WHITE, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, SW_FONT_M, 0);
    lv_obj_set_style_text_color(v, SW_COLOR_WHITE, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

/* Last path component (used as the script app's ID). */
static const char *_path_basename(const char *p)
{
    if (!p) return "";
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* ── Uninstall confirm (reference _uninstall_confirm_cb) ── */
static void _delete_confirm_cb(int idx, void *user_data)
{
    (void)user_data;
    if (idx != 1) return;   /* only "Uninstall" (0 = Cancel) */
    const app_entry_t *a = app_registry_get(s_info_index);
    if (a && a->is_js) {
        ESP_LOGI(TAG, "Uninstalling app '%s'", a->name);
        js_app_registry_delete(a->js_index);
    }
    settings_ui_go(P_APPS);
}

static void _delete_btn_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    const char *acts[] = { "Cancel", "Uninstall" };
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Deleting this app will also remove all of its data.");
    action_sheet_open_ex(SW_ICON_WARNING,
                         msg, acts, 2, true, _delete_confirm_cb, NULL);
}

/* ── Clear data confirm (reference _clear_data_confirm_cb) ── */
static void _clear_data_confirm_cb(int idx, void *user_data)
{
    (void)user_data;
    if (idx != 1) return;   /* only "Clear data" */
    const app_entry_t *a = app_registry_get(s_info_index);
    if (a && a->is_js) js_app_registry_clear_data(a->js_index);
    settings_ui_go(P_APP_INFO);   /* refresh: button becomes disabled */
}

static void _clear_data_btn_cb(lv_event_t *e)
{
    (void)e;
    settings_ui_vibe();
    const char *acts[] = { "Cancel", "Clear data" };
    action_sheet_open_ex(SW_ICON_WARNING,
                         "Clear this app's data? This cannot be undone.",
                         acts, 2, true, _clear_data_confirm_cb, NULL);
}

static void _build_app_info_page(void)
{
    lv_obj_t *list = settings_ui_list();
    const app_entry_t *a = app_registry_get(s_info_index);
    if (!a) return;

    const js_app_entry_t *je = a->is_js ? js_app_registry_get(a->js_index) : NULL;

    /* ── Header + info panel (eos_list_add_container: bg 0x242424,
     *    radius 25, pad 24, margin_bottom 8, COLUMN flex). ─────── */
    lv_obj_t *panel = lv_obj_create(list);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_NONE);
    lv_obj_set_size(panel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, SW_THEME_SECONDARY, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, EOS_WX(24), 0);
    lv_obj_set_style_margin_bottom(panel, EOS_WY(8), 0);
    lv_obj_set_style_align(panel, LV_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(panel, SW_LIST_RADIUS, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    /* Uniform vertical gap between the icon header and each info row. */
    lv_obj_set_style_pad_row(panel, EOS_WY(12), 0);

    /* Header row (eos_row_create): 100px circular icon + name (right). */
    lv_obj_t *hdr = lv_obj_create(panel);
    lv_obj_remove_style_all(hdr);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _app_circle_image(hdr, a->icon, EOS_WX(100));
    if (a->name) {
        lv_obj_t *nm = lv_label_create(hdr);
        lv_label_set_text(nm, a->name);
        lv_obj_set_style_text_font(nm, SW_FONT_L, 0);
        lv_obj_set_style_text_color(nm, SW_COLOR_WHITE, 0);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_flex_grow(nm, 1);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

    /* Info rows (reference: ID / Author / Version / SDK). */
    if (je) {
        _app_info_row(panel, "ID", je->dir ? _path_basename(je->dir) : "-");
        _app_info_row(panel, "Author", je->author ? je->author : "-");
        _app_info_row(panel, "Version", je->version ? je->version : "-");
        char sdk_buf[32];
        snprintf(sdk_buf, sizeof(sdk_buf), "Min: %d, Target: %d", 0, 0);
        _app_info_row(panel, "SDK", sdk_buf);
    } else {
        _app_info_row(panel, "Type", "System App");
        _app_info_row(panel, "ID", a->id ? a->id : "-");
    }

    /* ── Description (eos_list_add_title_container: transparent outer,
     *    title top-left, inner 0x242424 panel with wrapped text). ── */
    if (je && je->description && je->description[0]) {
        lv_obj_t *outer = lv_obj_create(list);
        lv_obj_remove_style_all(outer);
        lv_obj_set_size(outer, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *tl = lv_label_create(outer);
        lv_obj_set_width(tl, lv_pct(100));
        lv_obj_set_style_text_font(tl, SW_FONT_S, 0);
        lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(tl, "Description");
        lv_obj_set_style_text_color(tl, SW_COLOR_WHITE, 0);
        lv_obj_set_style_margin_hor(tl, EOS_WX(24), 0);
        lv_obj_set_style_margin_ver(tl, EOS_WY(8), 0);

        lv_obj_t *dpanel = lv_obj_create(outer);
        lv_obj_remove_flag(dpanel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(dpanel, LV_DIR_NONE);
        lv_obj_set_size(dpanel, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(dpanel, SW_THEME_SECONDARY, 0);
        lv_obj_set_style_border_width(dpanel, 0, 0);
        lv_obj_set_style_pad_all(dpanel, EOS_WX(24), 0);
        lv_obj_set_style_margin_bottom(dpanel, EOS_WY(8), 0);
        lv_obj_set_style_align(dpanel, LV_ALIGN_CENTER, 0);
        lv_obj_set_style_radius(dpanel, SW_LIST_RADIUS, 0);
        lv_obj_set_style_shadow_width(dpanel, 0, 0);

        lv_obj_t *dl = lv_label_create(dpanel);
        lv_label_set_text(dl, je->description);
        lv_obj_set_style_text_font(dl, SW_FONT_S, 0);
        lv_obj_set_style_text_color(dl, SW_COLOR_WHITE, 0);
        lv_obj_set_width(dl, lv_pct(100));
        lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
    }

    /* Native apps: nothing destructive to do. */
    if (!je) {
        lv_obj_t *note = lv_label_create(list);
        lv_obj_set_width(note, lv_pct(100));
        lv_label_set_text(note, "System app — cannot be removed.");
        lv_obj_set_style_text_font(note, SW_FONT_S, 0);
        lv_obj_set_style_text_color(note, SW_COLOR_TEXT_GREY, 0);
        lv_obj_set_style_pad_left(note, EOS_WX(16), 0);
        lv_obj_set_style_pad_top(note, EOS_WY(8), 0);
        return;
    }

    /* ── Group gap before the action buttons. ── */
    sw_list_add_group_gap(list);

    /* ── Clear data (uniform pill). Enabled only when the app has
     *    declared app data (<app dir>/app_data) holding files. ── */
    bool has_data = (je && js_app_registry_has_data(a->js_index));
    lv_obj_t *cd = sw_button_add(list, "Clear data", SW_COLOR_RED,
                                 has_data ? _clear_data_btn_cb : NULL);
    if (!has_data) lv_obj_add_state(cd, LV_STATE_DISABLED);

    /* ── Small gap between the two pills. ── */
    sw_list_add_placeholder(list, SW_LIST_ROW_PAD);

    /* ── Uninstall (same style). ── */
    sw_button_add(list, "Uninstall", SW_COLOR_RED, _delete_btn_cb);
}

/* ====================================================================
 * Page builder
 * ==================================================================== */
void settings_apps_build(page_t p)
{
    switch (p) {
    case P_APPS:     _build_apps_page();      break;
    case P_APP_INFO: _build_app_info_page();  break;
    default:         break;
    }
}
