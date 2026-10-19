/**
 * @file lv_mem_custom.c
 * @brief LVGL custom stdlib memory core backed by ESP32-S3 PSRAM.
 *
 * With CONFIG_LV_USE_CUSTOM_MALLOC=y, LVGL selects LV_STDLIB_CUSTOM and expects
 * the low-level memory functions below to be provided by the application.
 *
 * Routing them through heap_caps_* with MALLOC_CAP_SPIRAM moves LVGL's whole
 * dynamic heap (widgets, styles, draw objects, caches) to PSRAM. This frees
 * internal DRAM so the DMA-capable LVGL draw buffer and the SPI/QSPI DMA
 * internals can stay in internal RAM, which is required for correct partial
 * refresh on the SH8601/CO5300 panel.
 */

#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "esp_heap_caps.h"

void lv_mem_init(void)
{
    /* ESP-IDF heap is initialized at boot; nothing to do here. */
}

void lv_mem_deinit(void)
{
    /* Nothing to do here. */
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL; /* Not supported with a heap_caps-backed allocator. */
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    if (size == 0) {
        size = 1; /* heap_caps_malloc(0) returns NULL; LVGL expects a usable pointer. */
    }
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    if (new_size == 0) {
        new_size = 1;
    }
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if (mon_p == NULL) {
        return;
    }

    mon_p->total_size        = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon_p->free_size         = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    mon_p->used_pct          = (mon_p->total_size == 0)
                               ? 0
                               : (uint8_t)(100 - (mon_p->free_size * 100) / mon_p->total_size);
    mon_p->free_cnt          = 0;
    mon_p->used_cnt          = 0;
    mon_p->max_used          = 0;
    mon_p->frag_pct          = 0;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
