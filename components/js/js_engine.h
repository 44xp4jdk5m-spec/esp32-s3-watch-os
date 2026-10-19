/*
 * Watch OS JavaScript engine — public API.
 */
#ifndef JS_ENGINE_H
#define JS_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "quickjs/quickjs.h"
#include "quickjs/quickjs-libc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque per-app engine instance. */
typedef struct js_engine js_engine_t;

typedef struct {
    size_t      heap_limit; /* per-app QuickJS heap cap in bytes (0 = none) */
    size_t      stack_size; /* per-app JS stack cap in bytes  (0 = default) */
    const char *info;       /* runtime info string (must outlive the engine) */
} js_engine_cfg_t;

/* Create an engine whose entire heap lives in PSRAM. Returns NULL on OOM. */
js_engine_t *js_engine_create(const js_engine_cfg_t *cfg);

/* Free context, runtime and force-free any leaked PSRAM blocks. */
void js_engine_destroy(js_engine_t *eng);

JSContext *js_engine_get_context(js_engine_t *eng);
JSRuntime *js_engine_get_runtime(js_engine_t *eng);

/* Evaluate `len` bytes of JS. Returns 0 on success, -1 on error (the
 * error is dumped to the log). If `ret` is non-NULL and evaluation
 * succeeds, it receives the result value (caller must JS_FreeValue it). */
int js_engine_eval(js_engine_t *eng, const char *code, size_t len,
                   const char *filename, JSValue *ret);

/* Drain pending promise jobs and run a GC cycle. Call from the app loop. */
void js_engine_poll(js_engine_t *eng);

/* Live PSRAM usage of this engine (tracked by the sandbox allocator). */
size_t   js_engine_allocated_bytes(js_engine_t *eng);
uint32_t js_engine_allocation_count(js_engine_t *eng);

/* Force-free every still-tracked PSRAM block (GPIO0 hard-exit guarantee). */
void js_engine_force_free_tracked(js_engine_t *eng);

#ifdef __cplusplus
}
#endif

#endif /* JS_ENGINE_H */
