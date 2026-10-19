/**
 * @file esp_system.h — host stub.
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void     esp_restart(void);
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
uint32_t esp_random(void);
void     esp_fill_random(void *buf, size_t len);
const char *esp_get_idf_version(void);

#ifdef __cplusplus
}
#endif
