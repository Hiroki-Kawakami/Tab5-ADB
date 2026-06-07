/*
 * Host FreeRTOS semaphore/mutex API. Built on the queue object (semphr_*
 * helpers in freertos_queue.c): a semaphore is a zero-item-size queue whose
 * "message count" is the token count. Binary/counting semaphores are created
 * empty; mutexes are created available. Recursive mutexes track owner + depth.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef QueueHandle_t SemaphoreHandle_t;

/* mutex_kind: 0 = not a mutex, 1 = mutex, 2 = recursive mutex */
SemaphoreHandle_t freertos_sem_create(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount,
                                      int mutex_kind);
BaseType_t  freertos_sem_take(SemaphoreHandle_t s, TickType_t xTicksToWait);
BaseType_t  freertos_sem_give(SemaphoreHandle_t s);
BaseType_t  freertos_sem_take_recursive(SemaphoreHandle_t s, TickType_t xTicksToWait);
BaseType_t  freertos_sem_give_recursive(SemaphoreHandle_t s);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s);

#define xSemaphoreCreateBinary()              freertos_sem_create(1, 0, 0)
#define xSemaphoreCreateCounting(max, init)   freertos_sem_create((max), (init), 0)
#define xSemaphoreCreateMutex()               freertos_sem_create(1, 1, 1)
#define xSemaphoreCreateRecursiveMutex()      freertos_sem_create(1, 1, 2)

#define xSemaphoreTake(s, ticks)              freertos_sem_take((s), (ticks))
#define xSemaphoreGive(s)                     freertos_sem_give((s))
#define xSemaphoreTakeRecursive(s, ticks)     freertos_sem_take_recursive((s), (ticks))
#define xSemaphoreGiveRecursive(s)            freertos_sem_give_recursive((s))
#define vSemaphoreDelete(s)                   vQueueDelete((s))

/* FromISR: no ISR context on the host → non-blocking equivalents. */
#define xSemaphoreGiveFromISR(s, woken)       (((void)(woken)), freertos_sem_give((s)))
#define xSemaphoreTakeFromISR(s, woken)       (((void)(woken)), freertos_sem_take((s), 0))

#ifdef __cplusplus
}
#endif
