/*
 * Watch OS — JavaScript app runner implementation (see header).
 */
#include "jsapp_run.h"

#include "jsapp_loading.h"
#include "js_app_registry.h"
#include "js_bind.h"
#include "js_engine.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "JSAPP";

#define JS_HEAP_LIMIT         (512 * 1024)   /* per-app QuickJS heap cap  */
#define JSAPP_LOADING_MIN_MS  900            /* min time the dots are shown */

static js_app_t *s_app = NULL;

/* In-flight load state. */
typedef struct {
    int                  index;
    lv_obj_t            *loading_scr;
    lv_obj_t            *prev_scr;   /* screen to restore on failure/cancel */
    lv_timer_t          *timer;
    uint32_t             t0_ms;
    jsapp_load_done_cb_t done_cb;
    void                *done_ud;
} jsapp_loading_t;

static jsapp_loading_t *s_loading = NULL;

/* Index of the JS app currently on screen (-1 = none / launcher / loading).
 * Consumed by the app-usage tracker to attribute screen time. */
static int s_active_index = -1;

/* Read the app's entry script from the SD card into PSRAM. */
static char *jsapp_read_script(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > (256 * 1024)) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { heap_caps_free(buf); return NULL; }
    buf[sz] = '\0';
    return buf;
}

/* Finish the deferred load: tear down the loading screen, fire the done
 * callback, free the load state.  `ok` selects success vs failure/cancel. */
static void jsapp_finish_load(jsapp_loading_t *L, bool ok)
{
    if (L->timer) { lv_timer_del(L->timer); L->timer = NULL; }

    if (!ok) {
        /* Restore the launcher/menu FIRST (make it active), then delete the
         * loading screen — never delete the ACTIVE screen. */
        if (L->prev_scr && lv_obj_is_valid(L->prev_scr))
            lv_screen_load(L->prev_scr);
    }
    if (L->loading_scr && lv_obj_is_valid(L->loading_scr)) {
        lv_obj_del(L->loading_scr);
        L->loading_scr = NULL;
    }

    jsapp_load_done_cb_t cb = L->done_cb;
    void *ud = L->done_ud;
    heap_caps_free(L);
    s_loading = NULL;

    if (cb)
        cb(ok ? JSAPP_LOAD_OK : JSAPP_LOAD_FAIL, ud);
}

/* Do the actual (synchronous) load: read script, create engine, eval.
 * Runs inside a normal lv_timer_handler pass, so the previous passes have
 * already animated the loading dots for the minimum display time. */
static void jsapp_do_load(void)
{
    jsapp_loading_t *L = s_loading;
    if (!L) return;

    const char *path = js_app_registry_entry_path(L->index);
    if (!path) { jsapp_finish_load(L, false); return; }

    char *code = jsapp_read_script(path);
    if (!code) {
        ESP_LOGE(TAG, "failed to load script %s", path);
        jsapp_finish_load(L, false);
        return;
    }

    /* Create the engine first — if it fails we restore the previous
     * (launcher) screen so LVGL never ends up with no active screen. */
    js_engine_cfg_t cfg = { .heap_limit = JS_HEAP_LIMIT, .stack_size = 0, .info = "jsapp" };
    js_engine_t *eng = js_engine_create(&cfg);
    if (!eng) {
        ESP_LOGE(TAG, "engine create failed (PSRAM free=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        heap_caps_free(code);
        jsapp_finish_load(L, false);
        return;
    }

    /* App root screen (created after the engine, before loading). */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 410, 502);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_app = heap_caps_calloc(1, sizeof(*s_app), MALLOC_CAP_SPIRAM);
    if (!s_app) {
        js_engine_destroy(eng);
        heap_caps_free(code);
        lv_obj_del(scr);
        jsapp_finish_load(L, false);
        return;
    }
    s_app->eng = eng;
    s_app->ctx = js_engine_get_context(eng);
    s_app->scr = scr;
    s_app->exit_requested = false;
    s_app->nvs_open = false;
    s_active_index = L->index;

    JS_SetRuntimeOpaque(js_engine_get_runtime(eng), s_app);
    js_bindings_install(s_app->ctx, s_app);

    /* Make the app screen active before evaluating, so OS.LVGL.screen()
     * and parent-less object creation see it.  The app screen is now the
     * ACTIVE screen, so it is safe to delete the (inactive) loading screen. */
    lv_screen_load(scr);
    if (L->loading_scr && lv_obj_is_valid(L->loading_scr)) {
        lv_obj_del(L->loading_scr);
        L->loading_scr = NULL;
    }

    if (js_engine_eval(eng, code, strlen(code), path, NULL) != 0)
        ESP_LOGE(TAG, "eval failed for %s", path);

    heap_caps_free(code);
    ESP_LOGI(TAG, "JS app started: %s", path);
    jsapp_finish_load(L, true);
}

/* Deferred loader — fires on a normal lv_timer_handler pass so the loading
 * dots animate (never re-enter lv_timer_handler from an event callback). */
static void jsapp_loader_tick(lv_timer_t *t)
{
    (void)t;
    jsapp_loading_t *L = s_loading;
    if (!L) return;

    /* Keep the dots spinning for at least the minimum display time. */
    if ((uint32_t)(esp_timer_get_time() / 1000) - L->t0_ms < JSAPP_LOADING_MIN_MS)
        return;

    jsapp_do_load();   /* finishes + frees s_loading internally */
}

bool jsapp_run_start(int index, jsapp_load_done_cb_t done_cb, void *user_data)
{
    if (s_loading || s_app)
        jsapp_run_destroy();

    if (!js_app_registry_get(index))
        return false;

    const js_app_entry_t *entry = js_app_registry_get(index);
    lv_obj_t *loading_scr = jsapp_loading_screen_create(entry ? entry->icon : NULL);

    s_loading = heap_caps_calloc(1, sizeof(*s_loading), MALLOC_CAP_SPIRAM);
    if (!s_loading) {
        lv_obj_del(loading_scr);
        return false;
    }

    /* The screen that was active (launcher grid or honeycomb menu).  It must
     * stay alive until the done callback fires — the caller only deletes it
     * on JSAPP_LOAD_OK. */
    s_loading->index       = index;
    s_loading->loading_scr = loading_scr;
    s_loading->prev_scr    = lv_screen_active();
    s_loading->t0_ms       = (uint32_t)(esp_timer_get_time() / 1000);
    s_loading->done_cb     = done_cb;
    s_loading->done_ud     = user_data;

    lv_screen_load(loading_scr);

    s_loading->timer = lv_timer_create(jsapp_loader_tick, 20, NULL);
    return true;
}

void jsapp_run_destroy(void)
{
    /* Cancel an in-flight load first (the done callback is NOT fired). */
    if (s_loading) {
        jsapp_loading_t *L = s_loading;
        if (L->timer) { lv_timer_del(L->timer); L->timer = NULL; }
        if (L->loading_scr && lv_obj_is_valid(L->loading_scr)) {
            if (L->prev_scr && lv_obj_is_valid(L->prev_scr))
                lv_screen_load(L->prev_scr);
            lv_obj_del(L->loading_scr);
        }
        L->loading_scr = NULL;
        heap_caps_free(L);
        s_loading = NULL;
        ESP_LOGI(TAG, "JS app load cancelled");
    }

    if (!s_app) return;

    /* Stop the app's LVGL timers (game loops) first so a due timer can
     * never fire after the QuickJS context is destroyed below. */
    js_bind_cleanup(s_app);

    /* Delete the screen first — this fires LV_EVENT_DELETE on every widget
     * so the JS event references are freed while the context is still valid. */
    if (s_app->scr && lv_obj_is_valid(s_app->scr))
        lv_obj_del(s_app->scr);
    s_app->scr = NULL;

    js_engine_destroy(s_app->eng);
    s_app->eng = NULL;
    s_app->ctx = NULL;

    if (s_app->nvs_open)
        nvs_close(s_app->nvs);

    heap_caps_free(s_app);
    s_app = NULL;
    s_active_index = -1;
    ESP_LOGI(TAG, "JS app stopped");
}

bool jsapp_run_is_loading(void)
{
    return s_loading != NULL;
}

bool jsapp_run_is_active(void)
{
    return s_app != NULL;
}

int jsapp_run_active_index(void)
{
    return s_active_index;
}

bool jsapp_run_exit_requested(void)
{
    return s_app && s_app->exit_requested;
}
