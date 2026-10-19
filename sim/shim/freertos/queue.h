/**
 * @file freertos/queue.h — minimal host stub.
 */
#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
void          vQueueDelete(QueueHandle_t q);
BaseType_t    xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t    xQueueSendToBack(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t    xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t    xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t    xQueuePeek(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t    xQueueReset(QueueHandle_t q);
UBaseType_t   uxQueueMessagesWaiting(QueueHandle_t q);

#ifdef __cplusplus
}
#endif
