/**
 * @file freertos/FreeRTOS.h — minimal host stub.
 *
 * The simulator runs the UI from a single-threaded SDL loop, so these
 * declarations exist only to let the real firmware sources compile.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned int UBaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(t)  ((uint32_t)(t))

#define configTICK_RATE_HZ 1000

/* Critical sections are no-ops: the sim is single threaded. */
#define taskENTER_CRITICAL(mux) do { (void)(mux); } while (0)
#define taskEXIT_CRITICAL(mux)  do { (void)(mux); } while (0)
#define portENTER_CRITICAL(mux) do { (void)(mux); } while (0)
#define portEXIT_CRITICAL(mux)  do { (void)(mux); } while (0)
#define portMAX_DELAY 0xFFFFFFFFu

#ifdef __cplusplus
}
#endif
