/**
 * @file esp_heap_caps.h — host stub (plain malloc, caps ignored).
 */
#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_EXEC      (1 << 0)
#define MALLOC_CAP_32BIT     (1 << 1)
#define MALLOC_CAP_8BIT      (1 << 2)
#define MALLOC_CAP_DMA       (1 << 3)
#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_INTERNAL  (1 << 11)
#define MALLOC_CAP_DEFAULT   (1 << 12)
#define MALLOC_CAP_IRAM_8BIT (1 << 13)
#define MALLOC_CAP_RETENTION (1 << 14)
#define MALLOC_CAP_RTCRAM    (1 << 15)

static inline void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return calloc(n, size); }
static inline void *heap_caps_realloc(void *p, size_t size, uint32_t caps) { (void)caps; return realloc(p, size); }
static inline void  heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 8u * 1024u * 1024u; }
static inline size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 4u * 1024u * 1024u; }
static inline size_t heap_caps_get_total_size(uint32_t caps) { (void)caps; return 8u * 1024u * 1024u; }
static inline size_t heap_caps_get_minimum_free_size(uint32_t caps) { (void)caps; return heap_caps_get_free_size(caps); }

#ifdef __cplusplus
}
#endif
