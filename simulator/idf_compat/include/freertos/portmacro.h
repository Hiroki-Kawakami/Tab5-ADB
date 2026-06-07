/*
 * Host port macros: critical sections, yields, ISR helpers.
 *
 * Critical sections (taskENTER_CRITICAL / portENTER_CRITICAL, and the ESP-IDF
 * spinlock form that takes a portMUX_TYPE*) all map to a single global recursive
 * mutex — the "big kernel lock". On real hardware a critical section is atomic
 * against every other task because only one runs per core; with pthreads tasks
 * run truly in parallel, so this global lock restores single-at-a-time semantics
 * for code inside critical sections. The mux argument is accepted and ignored.
 *
 * There are no interrupts on the host, so portYIELD_FROM_ISR and the *FromISR
 * helpers are no-ops, and the FromISR API variants (in queue.h / semphr.h /
 * task.h) just call their normal counterparts.
 */
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

void vPortEnterCritical(void);
void vPortExitCritical(void);

/* ESP-IDF spinlock type — accepted for source compatibility, otherwise unused. */
typedef struct { uint32_t owner; uint32_t count; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0, 0 }
#define portMUX_FREE_VAL            0
static inline void portMUX_INITIALIZE(portMUX_TYPE *m) { if (m) { m->owner = 0; m->count = 0; } }

#define taskENTER_CRITICAL(...)          vPortEnterCritical()
#define taskEXIT_CRITICAL(...)           vPortExitCritical()
#define taskENTER_CRITICAL_ISR(...)      vPortEnterCritical()
#define taskEXIT_CRITICAL_ISR(...)       vPortExitCritical()
#define portENTER_CRITICAL(...)          vPortEnterCritical()
#define portEXIT_CRITICAL(...)           vPortExitCritical()
#define portENTER_CRITICAL_ISR(...)      vPortEnterCritical()
#define portEXIT_CRITICAL_ISR(...)       vPortExitCritical()
#define portENTER_CRITICAL_SAFE(...)     vPortEnterCritical()
#define portEXIT_CRITICAL_SAFE(...)      vPortExitCritical()

/* No interrupts on the host: these only need to be syntactically valid. */
#define portYIELD()                      taskYIELD()
#define portYIELD_FROM_ISR(...)          ((void)0)
#define portYIELD_WITHIN_API()           taskYIELD()
#define portEND_SWITCHING_ISR(...)       ((void)0)
#define taskYIELD()                      sched_yield()
#define portDISABLE_INTERRUPTS()         ((void)0)
#define portENABLE_INTERRUPTS()          ((void)0)
#define taskDISABLE_INTERRUPTS()         ((void)0)
#define taskENABLE_INTERRUPTS()          ((void)0)

#define pdMS_TO_TICKS_FLOOR(ms)          pdMS_TO_TICKS(ms)
#define portYIELD_CORE(x)                ((void)0)

#include <sched.h>

#ifdef __cplusplus
}
#endif
