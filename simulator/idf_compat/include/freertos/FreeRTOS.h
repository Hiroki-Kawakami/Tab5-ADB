/*
 * Host FreeRTOS-API compatibility — core types, config and projdefs.
 *
 * This is NOT the vendored FreeRTOS kernel. The simulator does not run the real
 * scheduler; instead the FreeRTOS *API contract* is reimplemented on top of
 * native pthreads (the same philosophy as the esp_* shims — reimplement the
 * contract, don't port the implementation). Tasks are pthreads scheduled by the
 * host OS, so there is no vTaskStartScheduler(), no tick ISR, no signal
 * machinery and no main-thread constraint: xTaskCreate() works anywhere, anytime.
 *
 * See ../../README.md ("FreeRTOS API compat") for the design and the one real
 * semantic gap (single-core atomicity → critical sections use a global lock).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- core types --- */
typedef long          BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t      TickType_t;
typedef uint8_t       StackType_t;
#define portMAX_DELAY ((TickType_t)0xffffffffUL)

/* --- config (host). Tick rate matches the device (CONFIG_FREERTOS_HZ=100) so
 *     raw tick delays and pdMS_TO_TICKS() behave identically on both targets. */
#define configTICK_RATE_HZ        ((TickType_t)100)
#define configMAX_PRIORITIES      (25)
#define configMINIMAL_STACK_SIZE  ((uint16_t)2048)
#define configSTACK_DEPTH_TYPE    uint32_t

/* --- projdefs --- */
#define pdFALSE ((BaseType_t)0)
#define pdTRUE  ((BaseType_t)1)
#define pdPASS  pdTRUE
#define pdFAIL  pdFALSE

#define errQUEUE_EMPTY ((BaseType_t)0)
#define errQUEUE_FULL  ((BaseType_t)0)

#define portTICK_PERIOD_MS ((TickType_t)(1000 / configTICK_RATE_HZ))
#define portTICK_RATE_MS   portTICK_PERIOD_MS

#define pdMS_TO_TICKS(ms)   ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000))
#define pdTICKS_TO_MS(t)    ((TickType_t)(((uint64_t)(t) * 1000) / configTICK_RATE_HZ))

/* --- host heap (FreeRTOS allocator API; plain malloc underneath) --- */
void *pvPortMalloc(size_t xSize);
void *pvPortCalloc(size_t xNum, size_t xSize);
void  vPortFree(void *pv);

#include "freertos/portmacro.h"

#ifdef __cplusplus
}
#endif
