/**
 * @file freertos/semphr.h — minimal host stub.
 *
 * Mutexes are real (POSIX) because some firmware code takes/releases them
 * around state that the SDL loop touches.
 */
#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void     vSemaphoreDelete(SemaphoreHandle_t s);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTakeFromISR(SemaphoreHandle_t s, BaseType_t *hp);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *hp);

#ifdef __cplusplus
}
#endif
