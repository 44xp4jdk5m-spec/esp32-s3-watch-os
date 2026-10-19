/*
 * Watch OS JavaScript engine wrapper.
 *
 * Thin wrapper around QuickJS that:
 *   - routes ALL QuickJS allocations through PSRAM
 *     (heap_caps_malloc(MALLOC_CAP_SPIRAM)) so DRAM is preserved,
 *   - tracks every allocation in a per-engine linked list so the OS can
 *     force-free leaked blocks on a hard (GPIO0) app exit,
 *   - applies a per-app heap limit and stack limit,
 *   - exposes create / eval / poll / destroy lifecycle helpers.
 *
 * QuickJS runtimes are NOT thread-safe: a js_engine_t must be used from a
 * single task (the JS app task).  The wrapper therefore uses no locks.
 */
#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "js_engine.h"

static const char *TAG = "js";

/* ------------------------------------------------------------------------
 * PSRAM allocation tracker
 * ---------------------------------------------------------------------- */

/* Header placed immediately before every block we hand to QuickJS. */
typedef struct js_block {
    struct js_block *next;  /* intrusive list node (per-engine)          */
    size_t           size;  /* usable size requested by the caller       */
    uint32_t         magic; /* sanity tag: 'JSMM'                        */
} js_block_t;

#define JS_BLOCK_MAGIC 0x4A534D4Du /* "JSMM" */

struct js_engine {
    JSRuntime      *rt;
    JSContext      *ctx;
    js_block_t     *blocks;        /* head of tracked allocation list    */
    size_t          allocated;     /* sum of tracked usable bytes        */
    uint32_t        alloc_count;   /* number of live tracked blocks      */
    js_engine_cfg_t cfg;
};

/* ------------------------------------------------------------------------
 * PSRAM allocator callbacks (JSMallocFunctions)
 * ---------------------------------------------------------------------- */

static void *js_psram_malloc(JSMallocState *s, size_t size)
{
    js_engine_t *eng = (js_engine_t *)s->opaque;
    js_block_t *b;

    if (size == 0)
        size = 1;

    b = (js_block_t *)heap_caps_malloc(sizeof(js_block_t) + size,
                                       MALLOC_CAP_SPIRAM);
    if (!b)
        return NULL;

    b->size  = size;
    b->magic = JS_BLOCK_MAGIC;
    if (eng) {
        b->next = eng->blocks;
        eng->blocks = b;
        eng->allocated += size;
        eng->alloc_count++;
    }
    return (void *)(b + 1);
}

static void js_psram_free(JSMallocState *s, void *ptr)
{
    js_engine_t *eng = (js_engine_t *)s->opaque;
    js_block_t *b;
    js_block_t **pp;

    if (!ptr)
        return;

    b = (js_block_t *)ptr - 1;
    if (b->magic != JS_BLOCK_MAGIC) {
        /* Should never happen; do not corrupt the heap. */
        ESP_LOGE(TAG, "js_psram_free: bad magic on %p", ptr);
        return;
    }

    /* Unlink from the tracker (we expect the block to be present). */
    for (pp = &eng->blocks; *pp; pp = &(*pp)->next) {
        if (*pp == b) {
            *pp = b->next;
            eng->allocated -= b->size;
            eng->alloc_count--;
            break;
        }
    }

    b->magic = 0;
    heap_caps_free(b);
}

static void *js_psram_realloc(JSMallocState *s, void *ptr, size_t size)
{
    js_block_t *b;
    size_t old_size;
    void *new_ptr;

    if (!ptr)
        return js_psram_malloc(s, size);
    if (size == 0) {
        js_psram_free(s, ptr);
        return NULL;
    }

    b = (js_block_t *)ptr - 1;
    old_size = b->size;

    /* Allocate a fresh block, copy the minimum of old/new, free old. */
    new_ptr = js_psram_malloc(s, size);
    if (!new_ptr)
        return NULL;
    memcpy(new_ptr, ptr, old_size < size ? old_size : size);
    js_psram_free(s, ptr);
    return new_ptr;
}

static size_t js_psram_usable_size(const void *ptr)
{
    const js_block_t *b;

    if (!ptr)
        return 0;
    b = (const js_block_t *)ptr - 1;
    if (b->magic != JS_BLOCK_MAGIC)
        return 0;
    return b->size;
}

static const JSMallocFunctions js_psram_mf = {
    js_psram_malloc,
    js_psram_free,
    js_psram_realloc,
    js_psram_usable_size,
};

/* ------------------------------------------------------------------------
 * Engine lifecycle
 * ---------------------------------------------------------------------- */

js_engine_t *js_engine_create(const js_engine_cfg_t *cfg)
{
    js_engine_t *eng;

    if (!cfg)
        return NULL;

    eng = (js_engine_t *)heap_caps_calloc(1, sizeof(*eng), MALLOC_CAP_SPIRAM);
    if (!eng) {
        ESP_LOGE(TAG, "OOM: engine struct (PSRAM free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return NULL;
    }

    eng->cfg = *cfg;
    eng->blocks = NULL;
    eng->allocated = 0;
    eng->alloc_count = 0;

    eng->rt = JS_NewRuntime2(&js_psram_mf, eng);
    if (!eng->rt) {
        ESP_LOGE(TAG, "OOM: JS_NewRuntime2 (PSRAM free=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        heap_caps_free(eng);
        return NULL;
    }

    if (cfg->info)
        JS_SetRuntimeInfo(eng->rt, cfg->info);

    /* Per-app heap cap (0 / SIZE_MAX = unlimited). */
    if (cfg->heap_limit)
        JS_SetMemoryLimit(eng->rt, cfg->heap_limit);

    /* Per-app stack cap (0 = keep QuickJS default). */
    if (cfg->stack_size)
        JS_SetMaxStackSize(eng->rt, cfg->stack_size);

    eng->ctx = JS_NewContext(eng->rt);
    if (!eng->ctx) {
        ESP_LOGE(TAG, "OOM: JS_NewContext (PSRAM free=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        JS_FreeRuntime(eng->rt);
        heap_caps_free(eng);
        return NULL;
    }

    /* console.log / print / performance.now helpers.  Math, JSON, Date,
     * Promise, ... are all registered by the core engine itself. */
    js_std_add_helpers(eng->ctx, 0, NULL);

    return eng;
}

void js_engine_destroy(js_engine_t *eng)
{
    if (!eng)
        return;

    if (eng->ctx) {
        JS_FreeContext(eng->ctx);
        eng->ctx = NULL;
    }
    if (eng->rt) {
        JS_FreeRuntime(eng->rt);
        eng->rt = NULL;
    }

    /* Any allocation the engine leaked (should normally be none) is
     * force-freed here so an app exit can never leak PSRAM. */
    js_engine_force_free_tracked(eng);

    heap_caps_free(eng);
}

JSContext *js_engine_get_context(js_engine_t *eng)
{
    return eng ? eng->ctx : NULL;
}

JSRuntime *js_engine_get_runtime(js_engine_t *eng)
{
    return eng ? eng->rt : NULL;
}

/* ------------------------------------------------------------------------
 * Execution
 * ---------------------------------------------------------------------- */

int js_engine_eval(js_engine_t *eng, const char *code, size_t len,
                   const char *filename, JSValue *ret)
{
    JSValue result;

    if (!eng || !eng->ctx || !code)
        return -1;

    result = JS_Eval(eng->ctx, code, len,
                     filename ? filename : "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        js_std_dump_error(eng->ctx);
        JS_FreeValue(eng->ctx, result);
        return -1;
    }

    if (ret) {
        *ret = result; /* caller owns the value and must free it */
    } else {
        JS_FreeValue(eng->ctx, result);
    }
    return 0;
}

void js_engine_poll(js_engine_t *eng)
{
    if (!eng || !eng->ctx || !eng->rt)
        return;
    /* Drain promise jobs, then collect garbage. */
    js_std_loop(eng->ctx);
    JS_RunGC(eng->rt);
}

/* ------------------------------------------------------------------------
 * Sandbox memory accounting / hard-exit cleanup
 * ---------------------------------------------------------------------- */

size_t js_engine_allocated_bytes(js_engine_t *eng)
{
    return eng ? eng->allocated : 0;
}

uint32_t js_engine_allocation_count(js_engine_t *eng)
{
    return eng ? eng->alloc_count : 0;
}

void js_engine_force_free_tracked(js_engine_t *eng)
{
    js_block_t *b;
    js_block_t *next;
    uint32_t n = 0;

    if (!eng)
        return;

    b = eng->blocks;
    eng->blocks = NULL;
    while (b) {
        next = b->next;
        n++;
        b->magic = 0;
        heap_caps_free(b);
        b = next;
    }

    if (n) {
        ESP_LOGW(TAG, "force-freed %lu leaked blocks (%u bytes)",
                 (unsigned long)n, (unsigned)eng->allocated);
    }
    eng->allocated = 0;
    eng->alloc_count = 0;
}
