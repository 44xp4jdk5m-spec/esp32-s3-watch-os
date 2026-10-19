/**
 * @file freertos/task.h — minimal host stub.
 */
#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

void      vTaskDelay(TickType_t ticks);
void      vTaskDelayUntil(TickType_t *prev, TickType_t increment);
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t prio, TaskHandle_t *out);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core);
void      vTaskDelete(TaskHandle_t task);
void      vTaskSuspend(TaskHandle_t task);
void      vTaskResume(TaskHandle_t task);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t task);
uint32_t  uxTaskGetStackHighWaterMark(TaskHandle_t task);
void      taskYIELD(void);

#ifdef __cplusplus
}
#endif
