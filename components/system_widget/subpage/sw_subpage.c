/**
 * @file sw_subpage.c
 * @brief Reusable subpage engine (see sw_subpage.h).
 *
 * Extracted from the original settings_ui.c coordinator: it owns the
 * per-page screen + list, the persistent floating header, the slide
 * transitions and the scroll-scale effect.  Page hierarchies, titles and
 * lifecycle hooks stay in the owner (e.g. the Settings app).
 */

#include "sw_subpage.h"
#include "sw_list.h"
#include "sw_transition.h"
#include <stdlib.h>

struct sw_subpage {
    lv_obj_t       *scr;          /* current page screen */
    lv_obj_t       *list;         /* rows container on scr */
    lv_obj_t       *prev_screen;  /* screen to restore on destroy */
    sw_header_t    *hdr;          /* persistent floating header */
    lv_timer_t     *scroll_timer; /* scroll-scale effect on list */
    sw_subpage_build_fn build;
    void           *build_ud;
    sw_subpage_back_fn back;
    void           *back_ud;
};

/* The header back button was pressed → hand off to the owner. */
static void sw_subpage_back_cb(lv_event_t *e)
{
    sw_subpage_t *sp = lv_event_get_user_data(e);
    if (sp && sp->back) sp->back(sp->back_ud);
}

sw_subpage_t *sw_subpage_create(const char *title,
                                sw_subpage_back_fn back_fn, void *back_ud)
{
    sw_subpage_t *sp = calloc(1, sizeof(sw_subpage_t));
    if (!sp) return NULL;

    sp->prev_screen = lv_screen_active();
    sp->back        = back_fn;
    sp->back_ud     = back_ud;
    sp->hdr         = sw_header_create(title ? title : "",
                                       sw_subpage_back_cb, sp);
    return sp;
}

static void sw_subpage_build_screen(sw_subpage_t *sp, const char *title,
                                    sw_subpage_build_fn build, void *ud)
{
    /* Stop the previous page's scroll effect. */
    sw_scroll_effect_stop(sp->scroll_timer);
    sp->scroll_timer = NULL;

    /* New top-level screen + list for this page. */
    sp->scr  = sw_screen_create();
    sp->list = sw_list_create(sp->scr, SW_HDR_H);

    sp->build    = build;
    sp->build_ud = ud;

    if (sp->hdr) sw_header_set_title(sp->hdr, title);
    if (build)   build(sp->list, ud);

    sp->scroll_timer = sw_scroll_effect_start(sp->list);
}

void sw_subpage_show(sw_subpage_t *sp, const char *title,
                     sw_subpage_build_fn build, void *ud, bool back)
{
    if (!sp) return;
    sw_subpage_build_screen(sp, title, build, ud);
    if (sp->scr) sw_screen_load(sp->scr, back);
}

void sw_subpage_show_instant(sw_subpage_t *sp, const char *title,
                             sw_subpage_build_fn build, void *ud)
{
    if (!sp) return;
    sw_subpage_build_screen(sp, title, build, ud);
    if (sp->scr) sw_screen_load_instant(sp->scr);
}

void sw_subpage_refresh(sw_subpage_t *sp)
{
    if (!sp || !sp->list || !sp->build) return;

    sw_scroll_effect_stop(sp->scroll_timer);
    sp->scroll_timer = NULL;

    /* Save scroll position. */
    int32_t scroll_y = lv_obj_get_scroll_y(sp->list);

    lv_obj_clean(sp->list);
    sp->build(sp->list, sp->build_ud);

    /* Force a layout pass so scroll_bottom is accurate. */
    lv_obj_update_layout(sp->list);

    /* Restore scroll position (clamped). */
    int32_t max_scroll = lv_obj_get_scroll_bottom(sp->list);
    if (scroll_y > max_scroll) scroll_y = max_scroll;
    if (scroll_y < 0) scroll_y = 0;
    lv_obj_scroll_to_y(sp->list, scroll_y, LV_ANIM_OFF);

    sp->scroll_timer = sw_scroll_effect_start(sp->list);
}

lv_obj_t *sw_subpage_list(const sw_subpage_t *sp)
{
    return sp ? sp->list : NULL;
}

sw_header_t *sw_subpage_header(const sw_subpage_t *sp)
{
    return sp ? sp->hdr : NULL;
}

lv_obj_t *sw_subpage_clock_label(const sw_subpage_t *sp)
{
    return (sp && sp->hdr) ? sw_header_clock_label(sp->hdr) : NULL;
}

void sw_subpage_set_title(sw_subpage_t *sp, const char *title)
{
    if (sp && sp->hdr) sw_header_set_title(sp->hdr, title);
}

void sw_subpage_set_header_visible(sw_subpage_t *sp, bool visible)
{
    if (sp && sp->hdr) sw_header_set_visible(sp->hdr, visible);
}

void sw_subpage_destroy(sw_subpage_t *sp)
{
    if (!sp) return;

    sw_scroll_effect_stop(sp->scroll_timer);
    sp->scroll_timer = NULL;

    if (sp->hdr) {
        sw_header_destroy(sp->hdr);
        sp->hdr = NULL;
    }

    /* Restore the previous screen — auto_del deletes sp->scr (and its
     * children, incl. the list). */
    if (sp->prev_screen && lv_obj_is_valid(sp->prev_screen)) {
        lv_screen_load_anim(sp->prev_screen, LV_SCR_LOAD_ANIM_FADE_ON,
                            200, 0, true);
    }
    free(sp);
}
