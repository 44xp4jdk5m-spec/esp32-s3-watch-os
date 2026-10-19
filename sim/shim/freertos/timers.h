/**
 * @file freertos/timers.h — minimal host stub.
 */
#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t timer);

TimerHandle_t xTimerCreate(const char *name, TickType_t period,
                           UBaseType_t reload, void *id,
                           TimerCallbackFunction_t cb);
BaseType_t xTimerStart(TimerHandle_t t, TickType_t wait);
BaseType_t xTimerStop(TimerHandle_t t, TickType_t wait);
BaseType_t xTimerChangePeriod(TimerHandle_t t, TickType_t period, TickType_t wait);
BaseType_t xTimerReset(TimerHandle_t t, TickType_t wait);
BaseType_t xTimerDelete(TimerHandle_t t, TickType_t wait);

#ifdef __cplusplus
}
#endif
