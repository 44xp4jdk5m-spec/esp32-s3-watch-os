/**
 * @file activity_pages.h
 * @brief Activity app screen builders (ported from my_ui / SquareLine).
 *
 * The four screens are built once when the app is created and kept alive
 * for the whole app lifetime — swiping between them only switches the
 * active LVGL screen, so no screen is ever destroyed mid-session.
 *
 *   - ACTIVITY_PAGE_RINGS   : the 3-ring "Activity" summary screen
 *   - ACTIVITY_PAGE_MOVE    : Move detail page    (red theme)
 *   - ACTIVITY_PAGE_EXERCISE: Exercise detail page (green theme)
 *   - ACTIVITY_PAGE_STAND   : Stand detail page   (blue theme)
 *
 * The detail pages share one layout builder driven by a per-page theme
 * table; every repetitive group (arcs, gridlines, axis labels, hourly
 * bars, ...) is built with a for-loop instead of unrolled code.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Max number of gesture-target objects stored per page. */
#define ACTIVITY_GESTURE_MAX   8

/** Max number of hourly bars a detail page can hold. */
#define ACTIVITY_MAX_BARS      12

typedef enum {
    ACTIVITY_PAGE_RINGS = 0,
    ACTIVITY_PAGE_MOVE,
    ACTIVITY_PAGE_EXERCISE,
    ACTIVITY_PAGE_STAND,
    ACTIVITY_PAGE_COUNT,
} activity_page_t;

/**
 * @brief Load the app's image icons from SPIFFS into PSRAM.
 * Idempotent — safe to call on every app create; icons are kept for the
 * session (the exit fade still renders the page, so they are not freed).
 */
void activity_pages_load_icons(void);

/** Ring index inside the `arcs[]` array (consistent on every page). */
typedef enum {
    ACTIVITY_RING_MOVE = 0,
    ACTIVITY_RING_EXERCISE,
    ACTIVITY_RING_STAND,
} activity_ring_t;

/** Widget handles of one built page. */
typedef struct {
    lv_obj_t *scr;                      /* the page screen               */
    lv_obj_t *time;                     /* live clock label              */
    lv_obj_t *arcs[3];                  /* [move, exercise, stand] rings */
    lv_obj_t *title;                    /* page title ("Move" ...)       */
    lv_obj_t *percent;                  /* percentage label              */
    lv_obj_t *value;                    /* big value label ("375/")      */
    lv_obj_t *unit;                     /* goal+unit label ("500\nCAL")  */
    int       goal;                     /* 500 / 30 / 12                 */
    const char *unit_suffix;            /* "CAL" / "MIN" / "HR"          */
    float     bar_hour[ACTIVITY_MAX_BARS]; /* absolute hour of each bar  */
    int       bar_count;
    lv_obj_t *chart_labels[4];          /* 6 h section labels            */
    lv_obj_t *chart_marks[4];           /* dotted x-axis markings        */
    lv_obj_t *bars[ACTIVITY_MAX_BARS];  /* hourly bars                   */
    lv_obj_t *gesture[ACTIVITY_GESTURE_MAX]; /* NULL-terminated: objects
                                                that should detect swipes */
} activity_page_ui_t;

/**
 * @brief Build the 3-ring Activity summary screen.
 * @param out  Receives the widget handles (must be non-NULL).
 * @return The created screen object (also stored in out->scr).
 */
lv_obj_t *activity_pages_build_rings(activity_page_ui_t *out);

/**
 * @brief Build one of the three detail pages (move/exercise/stand).
 * @param page  ACTIVITY_PAGE_MOVE / _EXERCISE / _STAND.
 * @param out   Receives the widget handles (must be non-NULL).
 * @return The created screen object (also stored in out->scr).
 */
lv_obj_t *activity_pages_build_detail(activity_page_t page, activity_page_ui_t *out);

/**
 * @brief Reposition the rolling 24 h chart for the given time.
 * @param ui        Detail page to update (Move/Exercise/Stand).
 * @param now_frac  Current time as fractional hours (e.g. 14.5).
 *
 * The chart's right edge is "now"; the four 6-hour section labels mark
 * the current block last and the previous three blocks before it, so the
 * label order changes as the day advances.
 */
void activity_pages_update_chart(activity_page_ui_t *ui, float now_frac);

/**
 * @brief Refresh a detail page from live metrics.
 * @param ui        Detail page to update.
 * @param value     Current value (kcal / minutes / hours).
 * @param goal      Goal (500 / 30 / 12).
 * @param percent   Ring percentage (0-100).
 * @param ring_pct  [move, exercise, stand] percentages for the header rings.
 */
void activity_pages_set_detail_values(activity_page_ui_t *ui, int value, int goal,
                                      int percent, const int ring_pct[3]);

#ifdef __cplusplus
}
#endif
