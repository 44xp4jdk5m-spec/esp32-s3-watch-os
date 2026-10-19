/*
 * Watch OS — JavaScript bindings implementation (see js_bind.h).
 */
#include "js_bind.h"

#include "fonts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "hw_imu.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LV_FONT_DECLARE(ui_font_QilkaBold42);
LV_FONT_DECLARE(ui_font_NotoSansCondensedBold80);

static const char *TAG = "JSBIND";

/* ── Font table (OS.LVGL.FONT ids) ────────────────────────────────── */
static const lv_font_t *const g_fonts[] = {
    &noto_font_14,                       /* FONT_SMALL      = 0 */
    &noto_font_24,                       /* FONT_MED_SMALL  = 1 */
    &ui_font_QilkaBold42,                /* FONT_MEDIUM     = 2 */
    &noto_font_48,                       /* FONT_LARGE      = 3 */
    &ui_font_NotoSansCondensedBold80,    /* FONT_HUGE       = 4 */
};
#define FONT_COUNT (int)(sizeof(g_fonts) / sizeof(g_fonts[0]))

/* ── LVGL object wrapper class ────────────────────────────────────── */
static JSClassID js_lvgl_obj_class_id;

typedef struct js_obj_evt {
    struct js_obj_evt *next;
    lv_event_code_t    code;
    JSValue            func;
} js_obj_evt_t;

typedef struct js_obj_data {
    js_app_t     *app;
    js_obj_evt_t *evts;
} js_obj_data_t;

typedef struct js_anim_ctx {
    JSContext *ctx;
    lv_obj_t  *obj;
    JSValue    cb;
    int        pending;
    bool       done;
} js_anim_ctx_t;

/* ── forward decls ────────────────────────────────────────────────── */
static JSValue js_lvgl_screen(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
static JSValue js_lvgl_create(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
static JSValue js_lvgl_label(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);
static JSValue js_lvgl_anim(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);
static JSValue js_lvgl_color(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);
static JSValue js_lvgl_timer(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);
static JSValue js_lvgl_pointer(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv);

/* ── object unwrap / wrap ─────────────────────────────────────────── */
static lv_obj_t *js_obj_from_value(JSContext *ctx, JSValueConst v)
{
    if (!JS_IsObject(v))
        return NULL;
    return (lv_obj_t *)JS_GetOpaque2(ctx, v, js_lvgl_obj_class_id);
}

static JSValue js_obj_get_this(JSContext *ctx, JSValueConst this_val,
                               lv_obj_t **out)
{
    *out = js_obj_from_value(ctx, this_val);
    if (!*out)
        return JS_ThrowTypeError(ctx, "expected an LVGL object");
    return JS_UNDEFINED;
}

/* Event dispatch: call a stored JS function (with optional gesture dir). */
static void js_dispatch(js_app_t *app, JSValue func, const char *dir)
{
    JSValue ret;
    if (dir) {
        JSValue arg = JS_NewString(app->ctx, dir);
        ret = JS_Call(app->ctx, func, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(app->ctx, arg);
    } else {
        ret = JS_Call(app->ctx, func, JS_UNDEFINED, 0, NULL);
    }
    if (JS_IsException(ret))
        js_std_dump_error(app->ctx);
    JS_FreeValue(app->ctx, ret);
}

static const char *js_gesture_dir_str(lv_dir_t dir)
{
    switch (dir) {
        case LV_DIR_LEFT:   return "left";
        case LV_DIR_RIGHT:  return "right";
        case LV_DIR_TOP:    return "up";
        case LV_DIR_BOTTOM: return "down";
        default:            return "none";
    }
}

static void js_lvgl_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    js_obj_data_t *d = (js_obj_data_t *)lv_obj_get_user_data(obj);
    if (!d || !d->app)
        return;

    if (code == LV_EVENT_DELETE) {
        js_obj_evt_t *ev = d->evts;
        while (ev) {
            js_obj_evt_t *nx = ev->next;
            JS_FreeValue(d->app->ctx, ev->func);
            heap_caps_free(ev);
            ev = nx;
        }
        heap_caps_free(d);
        return;
    }

    for (js_obj_evt_t *ev = d->evts; ev; ev = ev->next) {
        if (ev->code != code)
            continue;
        if (code == LV_EVENT_GESTURE) {
            lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
            js_dispatch(d->app, ev->func, js_gesture_dir_str(dir));
        } else {
            js_dispatch(d->app, ev->func, NULL);
        }
    }
}

/* Allocate a JS wrapper + attach the event plumbing for an lv_obj. */
static JSValue js_wrap_obj(JSContext *ctx, js_app_t *app, lv_obj_t *obj)
{
    JSValue j = JS_NewObjectClass(ctx, js_lvgl_obj_class_id);
    if (JS_IsException(j))
        return j;

    JS_SetOpaque(j, obj);

    js_obj_data_t *d = (js_obj_data_t *)heap_caps_calloc(1, sizeof(*d),
                                                         MALLOC_CAP_SPIRAM);
    if (d) {
        d->app = app;
        lv_obj_set_user_data(obj, d);
        lv_obj_add_event_cb(obj, js_lvgl_event_cb, LV_EVENT_ALL, NULL);
        /* Let a swipe anywhere (cell, panel, header…) bubble up to an
         * ancestor gesture handler (normally the app screen). Without
         * this, LV_EVENT_GESTURE only reaches the exact pressed object. */
        lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    return j;
}

/* ── animation plumbing ───────────────────────────────────────────── */
static void js_anim_exec_x(void *var, int32_t v)     { lv_obj_set_x((lv_obj_t *)var, v); }
static void js_anim_exec_y(void *var, int32_t v)     { lv_obj_set_y((lv_obj_t *)var, v); }
static void js_anim_exec_scale(void *var, int32_t v) { lv_obj_set_style_transform_scale((lv_obj_t *)var, v, 0); }
static void js_anim_exec_opa(void *var, int32_t v)   { lv_obj_set_style_opa((lv_obj_t *)var, v, 0); }

static void js_anim_done(js_anim_ctx_t *ac, bool call_cb)
{
    if (--ac->pending > 0)
        return;
    if (ac->done)
        return;
    ac->done = true;

    JSContext *ctx = ac->ctx;
    if (call_cb && JS_IsFunction(ctx, ac->cb)) {
        JSValue ret = JS_Call(ctx, ac->cb, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(ret))
            js_std_dump_error(ctx);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, ac->cb);
    heap_caps_free(ac);
}

static void js_anim_ready_cb(lv_anim_t *a)   { js_anim_done((js_anim_ctx_t *)lv_anim_get_user_data(a), true); }
static void js_anim_deleted_cb(lv_anim_t *a) { js_anim_done((js_anim_ctx_t *)lv_anim_get_user_data(a), false); }

static lv_anim_path_cb_t js_easing(int e)
{
    switch (e) {
        case 1:  return lv_anim_path_ease_out;
        case 2:  return lv_anim_path_ease_in_out;
        case 3:  return lv_anim_path_overshoot;
        default: return lv_anim_path_linear;
    }
}

static void js_anim_start(lv_obj_t *obj, int32_t start, int32_t end, int32_t dur,
                          int easing, lv_anim_exec_xcb_t exec, js_anim_ctx_t *ac)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, start, end);
    lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, js_easing(easing));
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_ready_cb(&a, js_anim_ready_cb);
    lv_anim_set_deleted_cb(&a, js_anim_deleted_cb);
    lv_anim_set_user_data(&a, ac);
    lv_anim_start(&a);
}

/* ── OS.LVGL functions ────────────────────────────────────────────── */
static JSValue js_lvgl_screen(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    js_app_t *app = (js_app_t *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    (void)this_val; (void)argc; (void)argv;
    lv_obj_t *scr = app ? app->scr : lv_screen_active();
    return js_wrap_obj(ctx, app, scr);
}

static JSValue js_lvgl_create(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    js_app_t *app = (js_app_t *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    (void)this_val;
    lv_obj_t *parent = (argc > 0) ? js_obj_from_value(ctx, argv[0]) : NULL;
    lv_obj_t *obj = lv_obj_create(parent ? parent : lv_screen_active());
    return js_wrap_obj(ctx, app, obj);
}

static JSValue js_lvgl_label(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    js_app_t *app = (js_app_t *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    (void)this_val;
    lv_obj_t *parent = (argc > 0) ? js_obj_from_value(ctx, argv[0]) : NULL;
    lv_obj_t *obj = lv_label_create(parent ? parent : lv_screen_active());
    if (argc > 1) {
        const char *s = JS_ToCString(ctx, argv[1]);
        if (s) {
            lv_label_set_text(obj, s);
            JS_FreeCString(ctx, s);
        }
    }
    return js_wrap_obj(ctx, app, obj);
}

static JSValue js_lvgl_color(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    int32_t r = 0, g = 0, b = 0;
    if (argc > 0) JS_ToInt32(ctx, &r, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &g, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &b, argv[2]);
    return JS_NewInt32(ctx, ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
}

static JSValue js_lvgl_anim(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "anim(obj, props, duration, easing[, cb])");

    lv_obj_t *obj = js_obj_from_value(ctx, argv[0]);
    if (!obj)
        return JS_ThrowTypeError(ctx, "anim: arg 0 is not an LVGL object");

    int32_t duration = 0, easing = 0;
    JS_ToInt32(ctx, &duration, argv[2]);
    JS_ToInt32(ctx, &easing, argv[3]);
    if (duration < 1) duration = 1;

    JSValue cb = (argc > 4) ? argv[4] : JS_UNDEFINED;
    if (JS_IsUndefined(cb)) cb = JS_NULL;

    js_anim_ctx_t *ac = (js_anim_ctx_t *)heap_caps_calloc(1, sizeof(*ac),
                                                          MALLOC_CAP_SPIRAM);
    if (!ac) {
        JS_FreeValue(ctx, cb);
        return JS_EXCEPTION;
    }
    ac->ctx = ctx;
    ac->obj = obj;
    ac->cb = JS_DupValue(ctx, cb);
    ac->pending = 0;
    ac->done = false;

    JSValue v;
    int32_t n;
    v = JS_GetPropertyStr(ctx, argv[1], "x");
    if (JS_IsNumber(v)) { JS_ToInt32(ctx, &n, v); js_anim_start(obj, lv_obj_get_x(obj), n, duration, easing, js_anim_exec_x, ac); ac->pending++; }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[1], "y");
    if (JS_IsNumber(v)) { JS_ToInt32(ctx, &n, v); js_anim_start(obj, lv_obj_get_y(obj), n, duration, easing, js_anim_exec_y, ac); ac->pending++; }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[1], "scale");
    if (JS_IsNumber(v)) { JS_ToInt32(ctx, &n, v); js_anim_start(obj, lv_obj_get_style_transform_scale_x(obj, 0), n, duration, easing, js_anim_exec_scale, ac); ac->pending++; }
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[1], "opacity");
    if (JS_IsNumber(v)) { JS_ToInt32(ctx, &n, v); js_anim_start(obj, lv_obj_get_style_opa(obj, 0), n, duration, easing, js_anim_exec_opa, ac); ac->pending++; }
    JS_FreeValue(ctx, v);

    if (ac->pending == 0) {
        /* nothing to animate — call back immediately */
        JS_FreeValue(ctx, ac->cb);
        heap_caps_free(ac);
        if (JS_IsFunction(ctx, cb)) {
            JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(ret)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, ret);
        }
    }
    return JS_UNDEFINED;
}

/* ── object methods ───────────────────────────────────────────────── */
#define OBJ_METHODS_BEGIN \
    static JSValue js_m_##NAME(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)

/* Simple setters/getters are implemented with macros to reduce boilerplate. */
static JSValue js_m_setPos(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t x = 0, y = 0;
    if (argc > 0) JS_ToInt32(ctx, &x, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &y, argv[1]);
    lv_obj_set_pos(o, x, y);
    return JS_UNDEFINED;
}
static JSValue js_m_setSize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t w = 0, h = 0;
    if (argc > 0) JS_ToInt32(ctx, &w, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &h, argv[1]);
    lv_obj_set_size(o, w, h);
    return JS_UNDEFINED;
}
static JSValue js_m_setRadius(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t r = 0; if (argc > 0) JS_ToInt32(ctx, &r, argv[0]);
    lv_obj_set_style_radius(o, r, 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setBgColor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t c = 0; if (argc > 0) JS_ToInt32(ctx, &c, argv[0]);
    lv_obj_set_style_bg_color(o, lv_color_hex((uint32_t)c), 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setBgOpa(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t v = 255; if (argc > 0) JS_ToInt32(ctx, &v, argv[0]);
    lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setOpa(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t v = 255; if (argc > 0) JS_ToInt32(ctx, &v, argv[0]);
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setTextColor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t c = 0; if (argc > 0) JS_ToInt32(ctx, &c, argv[0]);
    lv_obj_set_style_text_color(o, lv_color_hex((uint32_t)c), 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    if (argc > 0) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) { lv_label_set_text(o, s); JS_FreeCString(ctx, s); }
    }
    return JS_UNDEFINED;
}
static JSValue js_m_setFont(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t id = 0; if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= FONT_COUNT) id = 0;
    lv_obj_set_style_text_font(o, g_fonts[id], 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setTextAlign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t a = 0; if (argc > 0) JS_ToInt32(ctx, &a, argv[0]);
    lv_obj_set_style_text_align(o, (lv_text_align_t)a, 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setBorderWidth(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t w = 0; if (argc > 0) JS_ToInt32(ctx, &w, argv[0]);
    lv_obj_set_style_border_width(o, w, 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setPadAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t p = 0; if (argc > 0) JS_ToInt32(ctx, &p, argv[0]);
    lv_obj_set_style_pad_all(o, p, 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setShadowWidth(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t w = 0; if (argc > 0) JS_ToInt32(ctx, &w, argv[0]);
    lv_obj_set_style_shadow_width(o, w, 0);
    return JS_UNDEFINED;
}
static JSValue js_m_alignTo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    if (argc < 2) return JS_UNDEFINED;
    lv_obj_t *base = js_obj_from_value(ctx, argv[0]);
    int32_t align = 0, x = 0, y = 0;
    JS_ToInt32(ctx, &align, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &x, argv[2]);
    if (argc > 3) JS_ToInt32(ctx, &y, argv[3]);
    lv_obj_align_to(o, base, (lv_align_t)align, x, y);
    return JS_UNDEFINED;
}
static JSValue js_m_center(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    (void)argc; (void)argv;
    lv_obj_center(o);
    return JS_UNDEFINED;
}
static JSValue js_m_addFlag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t f = 0; if (argc > 0) JS_ToInt32(ctx, &f, argv[0]);
    lv_obj_add_flag(o, (lv_obj_flag_t)f);
    return JS_UNDEFINED;
}
static JSValue js_m_clearFlag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t f = 0; if (argc > 0) JS_ToInt32(ctx, &f, argv[0]);
    lv_obj_remove_flag(o, (lv_obj_flag_t)f);
    return JS_UNDEFINED;
}
static JSValue js_m_setScrollBarMode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t m = 0; if (argc > 0) JS_ToInt32(ctx, &m, argv[0]);
    lv_obj_set_scrollbar_mode(o, (lv_scrollbar_mode_t)m);
    return JS_UNDEFINED;
}
static JSValue js_m_centerPivot(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    (void)argc; (void)argv;
    lv_obj_set_style_transform_pivot_x(o, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(o, lv_pct(50), 0);
    return JS_UNDEFINED;
}
static JSValue js_m_setScale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    int32_t s = 256; if (argc > 0) JS_ToInt32(ctx, &s, argv[0]);
    lv_obj_set_style_transform_scale(o, s, 0);
    return JS_UNDEFINED;
}
static JSValue js_m_getX(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ lv_obj_t *o; js_obj_get_this(ctx, this_val, &o); (void)argc; (void)argv; return JS_NewInt32(ctx, lv_obj_get_x(o)); }
static JSValue js_m_getY(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ lv_obj_t *o; js_obj_get_this(ctx, this_val, &o); (void)argc; (void)argv; return JS_NewInt32(ctx, lv_obj_get_y(o)); }
static JSValue js_m_getW(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ lv_obj_t *o; js_obj_get_this(ctx, this_val, &o); (void)argc; (void)argv; return JS_NewInt32(ctx, lv_obj_get_width(o)); }
static JSValue js_m_getH(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ lv_obj_t *o; js_obj_get_this(ctx, this_val, &o); (void)argc; (void)argv; return JS_NewInt32(ctx, lv_obj_get_height(o)); }
static JSValue js_m_del(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    (void)argc; (void)argv;
    lv_obj_del(o);
    return JS_UNDEFINED;
}
static JSValue js_m_on(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lv_obj_t *o; JSValue e = js_obj_get_this(ctx, this_val, &o); if (JS_IsException(e)) return e;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "on(event, callback)");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    lv_event_code_t code = LV_EVENT_LAST;
    if      (strcmp(name, "clicked")  == 0) code = LV_EVENT_CLICKED;
    else if (strcmp(name, "pressed")  == 0) code = LV_EVENT_PRESSED;
    else if (strcmp(name, "released") == 0) code = LV_EVENT_RELEASED;
    else if (strcmp(name, "gesture")  == 0) code = LV_EVENT_GESTURE;
    else if (strcmp(name, "delete")   == 0) code = LV_EVENT_DELETE;
    JS_FreeCString(ctx, name);

    if (code == LV_EVENT_LAST)
        return JS_ThrowTypeError(ctx, "unknown event name");

    js_obj_data_t *d = (js_obj_data_t *)lv_obj_get_user_data(o);
    if (!d) return JS_ThrowTypeError(ctx, "object has no user data");

    js_obj_evt_t *ev = (js_obj_evt_t *)heap_caps_calloc(1, sizeof(*ev), MALLOC_CAP_SPIRAM);
    if (!ev) return JS_EXCEPTION;
    ev->code = code;
    ev->func = JS_DupValue(ctx, argv[1]);
    ev->next = d->evts;
    d->evts = ev;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_lvgl_obj_proto[] = {
    JS_CFUNC_DEF("setPos", 2, js_m_setPos),
    JS_CFUNC_DEF("setSize", 2, js_m_setSize),
    JS_CFUNC_DEF("setRadius", 1, js_m_setRadius),
    JS_CFUNC_DEF("setBgColor", 1, js_m_setBgColor),
    JS_CFUNC_DEF("setBgOpa", 1, js_m_setBgOpa),
    JS_CFUNC_DEF("setOpa", 1, js_m_setOpa),
    JS_CFUNC_DEF("setTextColor", 1, js_m_setTextColor),
    JS_CFUNC_DEF("setText", 1, js_m_setText),
    JS_CFUNC_DEF("setFont", 1, js_m_setFont),
    JS_CFUNC_DEF("setTextAlign", 1, js_m_setTextAlign),
    JS_CFUNC_DEF("setBorderWidth", 1, js_m_setBorderWidth),
    JS_CFUNC_DEF("setPadAll", 1, js_m_setPadAll),
    JS_CFUNC_DEF("setShadowWidth", 1, js_m_setShadowWidth),
    JS_CFUNC_DEF("alignTo", 4, js_m_alignTo),
    JS_CFUNC_DEF("center", 0, js_m_center),
    JS_CFUNC_DEF("addFlag", 1, js_m_addFlag),
    JS_CFUNC_DEF("clearFlag", 1, js_m_clearFlag),
    JS_CFUNC_DEF("setScrollBarMode", 1, js_m_setScrollBarMode),
    JS_CFUNC_DEF("centerPivot", 0, js_m_centerPivot),
    JS_CFUNC_DEF("setScale", 1, js_m_setScale),
    JS_CFUNC_DEF("getX", 0, js_m_getX),
    JS_CFUNC_DEF("getY", 0, js_m_getY),
    JS_CFUNC_DEF("getW", 0, js_m_getW),
    JS_CFUNC_DEF("getH", 0, js_m_getH),
    JS_CFUNC_DEF("del", 0, js_m_del),
    JS_CFUNC_DEF("on", 2, js_m_on),
};

static const JSCFunctionListEntry js_lvgl_funcs[] = {
    JS_CFUNC_DEF("screen", 0, js_lvgl_screen),
    JS_CFUNC_DEF("create", 1, js_lvgl_create),
    JS_CFUNC_DEF("label", 2, js_lvgl_label),
    JS_CFUNC_DEF("color", 3, js_lvgl_color),
    JS_CFUNC_DEF("anim", 5, js_lvgl_anim),
    JS_CFUNC_DEF("timer", 2, js_lvgl_timer),
    JS_CFUNC_DEF("pointer", 0, js_lvgl_pointer),
};

/* ── OS.System / OS.Storage ───────────────────────────────────────── */
static JSValue js_sys_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ (void)this_val; (void)argc; (void)argv; return JS_NewInt64(ctx, esp_timer_get_time() / 1000); }

static JSValue js_sys_exit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    js_app_t *app = (js_app_t *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    (void)this_val; (void)argc; (void)argv;
    if (app) app->exit_requested = true;
    return JS_UNDEFINED;
}

static void js_storage_ensure(js_app_t *app)
{
    if (app->nvs_open) return;
    if (nvs_open("jsapps", NVS_READWRITE, &app->nvs) == ESP_OK)
        app->nvs_open = true;
}

static JSValue js_storage_getInt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    js_app_t *app = (js_app_t *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    (void)this_val;
    int32_t out = 0;
    if (argc > 0 && app) {
        const char *key = JS_ToCString(ctx, argv[0]);
        if (key) {
            js_storage_ensure(app);
            if (app->nvs_open)
                nvs_get_i32(app->nvs, key, &out);
            JS_FreeCString(ctx, key);
        }
    }
    return JS_NewInt32(ctx, out);
}

static JSValue js_storage_setInt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    js_app_t *app = (js_app_t *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    (void)this_val;
    if (argc > 1 && app) {
        const char *key = JS_ToCString(ctx, argv[0]);
        int32_t val = 0;
        JS_ToInt32(ctx, &val, argv[1]);
        if (key) {
            js_storage_ensure(app);
            if (app->nvs_open)
                nvs_set_i32(app->nvs, key, val);
            JS_FreeCString(ctx, key);
        }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_sys_funcs[] = {
    JS_CFUNC_DEF("now", 0, js_sys_now),
    JS_CFUNC_DEF("exit", 0, js_sys_exit),
};

static const JSCFunctionListEntry js_storage_funcs[] = {
    JS_CFUNC_DEF("getInt", 1, js_storage_getInt),
    JS_CFUNC_DEF("setInt", 2, js_storage_setInt),
};

/* ── OS.Sensor (QMI8658 IMU) ─────────────────────────────────────── */
/* read()  -> { ax,ay,az (m/s²), gx,gy,gz (rad/s) } raw sample
 * tilt()  -> { roll, pitch } in degrees, 0° = watch face-up flat.
 *          roll  = atan2(ay, √(ax²+az²))  (tilt around the screen axis)
 *          pitch = atan2(ax, √(ay²+az²))  (tilt forward/back) */
static JSValue js_sensor_read(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    hw_imu_data_t d;
    JSValue o = JS_NewObject(ctx);
    if (hw_imu_read(&d) == ESP_OK) {
        JS_SetPropertyStr(ctx, o, "ax", JS_NewFloat64(ctx, d.accel_x));
        JS_SetPropertyStr(ctx, o, "ay", JS_NewFloat64(ctx, d.accel_y));
        JS_SetPropertyStr(ctx, o, "az", JS_NewFloat64(ctx, d.accel_z));
        JS_SetPropertyStr(ctx, o, "gx", JS_NewFloat64(ctx, d.gyro_x));
        JS_SetPropertyStr(ctx, o, "gy", JS_NewFloat64(ctx, d.gyro_y));
        JS_SetPropertyStr(ctx, o, "gz", JS_NewFloat64(ctx, d.gyro_z));
    }
    return o;
}

static JSValue js_sensor_tilt(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    double roll = 0.0, pitch = 0.0;
    hw_imu_data_t d;
    if (hw_imu_read(&d) == ESP_OK) {
        float ax = d.accel_x, ay = d.accel_y, az = d.accel_z;
        roll  = atan2f(ay, sqrtf(ax * ax + az * az)) * (180.0 / M_PI);
        pitch = atan2f(ax, sqrtf(ay * ay + az * az)) * (180.0 / M_PI);
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "roll",  JS_NewFloat64(ctx, roll));
    JS_SetPropertyStr(ctx, o, "pitch", JS_NewFloat64(ctx, pitch));
    return o;
}

static const JSCFunctionListEntry js_sensor_funcs[] = {
    JS_CFUNC_DEF("read", 0, js_sensor_read),
    JS_CFUNC_DEF("tilt", 0, js_sensor_tilt),
};

/* ── constants ────────────────────────────────────────────────────── */
static void js_set_int(JSContext *ctx, JSValue obj, const char *name, int v)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewInt32(ctx, v));
}

static void js_lvgl_install_constants(JSContext *ctx, JSValue lvgl)
{
    JSValue f = JS_NewObject(ctx);
    js_set_int(ctx, f, "SMALL", 0);
    js_set_int(ctx, f, "MED_SMALL", 1);
    js_set_int(ctx, f, "MEDIUM", 2);
    js_set_int(ctx, f, "LARGE", 3);
    js_set_int(ctx, f, "HUGE", 4);
    JS_SetPropertyStr(ctx, lvgl, "FONT", f);

    JSValue a = JS_NewObject(ctx);
    js_set_int(ctx, a, "CENTER", LV_ALIGN_CENTER);
    js_set_int(ctx, a, "TOP_LEFT", LV_ALIGN_TOP_LEFT);
    js_set_int(ctx, a, "TOP_MID", LV_ALIGN_TOP_MID);
    js_set_int(ctx, a, "TOP_RIGHT", LV_ALIGN_TOP_RIGHT);
    js_set_int(ctx, a, "BOTTOM_LEFT", LV_ALIGN_BOTTOM_LEFT);
    js_set_int(ctx, a, "BOTTOM_MID", LV_ALIGN_BOTTOM_MID);
    js_set_int(ctx, a, "BOTTOM_RIGHT", LV_ALIGN_BOTTOM_RIGHT);
    js_set_int(ctx, a, "LEFT_MID", LV_ALIGN_LEFT_MID);
    js_set_int(ctx, a, "RIGHT_MID", LV_ALIGN_RIGHT_MID);
    JS_SetPropertyStr(ctx, lvgl, "ALIGN", a);

    JSValue ta = JS_NewObject(ctx);
    js_set_int(ctx, ta, "LEFT", LV_TEXT_ALIGN_LEFT);
    js_set_int(ctx, ta, "CENTER", LV_TEXT_ALIGN_CENTER);
    js_set_int(ctx, ta, "RIGHT", LV_TEXT_ALIGN_RIGHT);
    JS_SetPropertyStr(ctx, lvgl, "TEXT_ALIGN", ta);

    JSValue fl = JS_NewObject(ctx);
    js_set_int(ctx, fl, "CLICKABLE", LV_OBJ_FLAG_CLICKABLE);
    js_set_int(ctx, fl, "SCROLLABLE", LV_OBJ_FLAG_SCROLLABLE);
    JS_SetPropertyStr(ctx, lvgl, "FLAG", fl);

    JSValue op = JS_NewObject(ctx);
    js_set_int(ctx, op, "TRANSP", LV_OPA_TRANSP);
    js_set_int(ctx, op, "COVER", LV_OPA_COVER);
    JS_SetPropertyStr(ctx, lvgl, "OPA", op);

    JSValue sb = JS_NewObject(ctx);
    js_set_int(ctx, sb, "OFF", LV_SCROLLBAR_MODE_OFF);
    js_set_int(ctx, sb, "ON", LV_SCROLLBAR_MODE_ON);
    JS_SetPropertyStr(ctx, lvgl, "SCROLLBAR", sb);

    JSValue an = JS_NewObject(ctx);
    js_set_int(ctx, an, "LINEAR", 0);
    js_set_int(ctx, an, "EASE_OUT", 1);
    js_set_int(ctx, an, "EASE_IN_OUT", 2);
    js_set_int(ctx, an, "OVERSHOOT", 3);
    JS_SetPropertyStr(ctx, lvgl, "ANIM", an);
}

/* ── OS.LVGL.timer — periodic JS callback (game loop) ───────────────
 * Creates an LVGL timer that calls the JS function every `period` ms.
 * The callback runs inside the LVGL task (lv_timer_handler), which is
 * exactly where all other JS executes, so it may freely touch LVGL
 * objects.  Timers are owned by the app and stopped by js_bind_cleanup()
 * when the app is torn down (before the QuickJS context is destroyed). */
typedef struct js_js_timer {
    struct js_js_timer *next;
    js_app_t     *app;
    lv_timer_t   *timer;
    JSValue       func;
} js_js_timer_t;

static js_js_timer_t *s_js_timers = NULL;

static void js_js_timer_cb(lv_timer_t *t)
{
    js_js_timer_t *jt = (js_js_timer_t *)lv_timer_get_user_data(t);
    if (!jt || !jt->app || !jt->app->ctx || jt->app->exit_requested)
        return;
    JSValue ret = JS_Call(jt->app->ctx, jt->func, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(ret))
        js_std_dump_error(jt->app->ctx);
    JS_FreeValue(jt->app->ctx, ret);
}

static JSValue js_lvgl_timer(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    js_app_t *app = (js_app_t *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    (void)this_val;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "timer(periodMs, callback)");
    int32_t period = 20;
    if (argc > 0) JS_ToInt32(ctx, &period, argv[0]);
    if (period < 5) period = 5;

    js_js_timer_t *jt = (js_js_timer_t *)heap_caps_calloc(1, sizeof(*jt),
                                                          MALLOC_CAP_SPIRAM);
    if (!jt)
        return JS_EXCEPTION;
    jt->app  = app;
    jt->func = JS_DupValue(ctx, argv[1]);
    jt->timer = lv_timer_create(js_js_timer_cb, (uint32_t)period, jt);
    if (!jt->timer) {
        JS_FreeValue(ctx, jt->func);
        heap_caps_free(jt);
        return JS_EXCEPTION;
    }
    jt->next = s_js_timers;
    s_js_timers = jt;
    return JS_UNDEFINED;
}

/* ── OS.LVGL.pointer — current touch position ───────────────────────
 * Returns { x, y, down } using the last latched state of the pointer
 * input device.  Works from a timer callback too (reads the indev's
 * stored point/state rather than lv_indev_active()). */
static JSValue js_lvgl_pointer(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    int32_t x = 0, y = 0, down = 0;
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            x = p.x;
            y = p.y;
            down = (lv_indev_get_state(indev) & LV_INDEV_STATE_PRESSED) ? 1 : 0;
            break;
        }
    }
    JSValue o = JS_NewObject(ctx);
    js_set_int(ctx, o, "x", x);
    js_set_int(ctx, o, "y", y);
    js_set_int(ctx, o, "down", down);
    return o;
}

/* Stop every LVGL timer belonging to `app` and release its JS callback.
 * Called from jsapp_run_destroy() BEFORE lv_obj_del(scr)/js_engine_destroy()
 * so a due timer can never fire after the context is freed. */
void js_bind_cleanup(js_app_t *app)
{
    if (!app)
        return;
    js_js_timer_t **pp = &s_js_timers;
    while (*pp) {
        js_js_timer_t *jt = *pp;
        if (jt->app != app) {
            pp = &jt->next;
            continue;
        }
        if (jt->timer)
            lv_timer_del(jt->timer);
        if (app->ctx)
            JS_FreeValue(app->ctx, jt->func);
        *pp = jt->next;
        heap_caps_free(jt);
    }
}

/* ── install ──────────────────────────────────────────────────────── */
void js_bindings_install(JSContext *ctx, js_app_t *app)
{
    /* The class ID is process-global and allocated once.  But the class
     * definition + prototype are PER-RUNTIME: each app launch creates a
     * fresh JSRuntime (js_engine_create → JS_NewRuntime2), so we MUST
     * re-register them here every time.  If we only registered on the first
     * runtime, the second launch would read class_proto[class_id] with
     * class_id >= rt->class_count (out of bounds into freed PSRAM) →
     * "Cache error / MMU entry fault" in JS_GetPropertyInternal during
     * the second eval. */
    static bool class_id_ready = false;
    if (!class_id_ready) {
        JS_NewClassID(&js_lvgl_obj_class_id);
        class_id_ready = true;
    }

    JSClassDef def = { 0 };
    def.class_name = "lvgl_obj";
    JS_NewClass(JS_GetRuntime(ctx), js_lvgl_obj_class_id, &def);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_lvgl_obj_proto,
                               (int)(sizeof(js_lvgl_obj_proto) / sizeof(js_lvgl_obj_proto[0])));
    JS_SetClassProto(ctx, js_lvgl_obj_class_id, proto);

    JSValue lvgl = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, lvgl, js_lvgl_funcs,
                               (int)(sizeof(js_lvgl_funcs) / sizeof(js_lvgl_funcs[0])));
    js_lvgl_install_constants(ctx, lvgl);

    JSValue sys = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sys, js_sys_funcs,
                               (int)(sizeof(js_sys_funcs) / sizeof(js_sys_funcs[0])));

    JSValue storage = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, storage, js_storage_funcs,
                               (int)(sizeof(js_storage_funcs) / sizeof(js_storage_funcs[0])));

    JSValue sensor = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sensor, js_sensor_funcs,
                               (int)(sizeof(js_sensor_funcs) / sizeof(js_sensor_funcs[0])));

    JSValue os = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, os, "LVGL", lvgl);
    JS_SetPropertyStr(ctx, os, "System", sys);
    JS_SetPropertyStr(ctx, os, "Storage", storage);
    JS_SetPropertyStr(ctx, os, "Sensor", sensor);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "OS", os);
    JS_FreeValue(ctx, global);
}
