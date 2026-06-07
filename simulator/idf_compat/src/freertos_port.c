/*
 * Host FreeRTOS port primitives: heap, time helpers, the global critical-section
 * lock, ticks, and scheduler stubs. See include/freertos/FreeRTOS.h for why the
 * real kernel is not used.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- heap (FreeRTOS allocator API over malloc) --- */
void *pvPortMalloc(size_t xSize)              { return malloc(xSize); }
void *pvPortCalloc(size_t xNum, size_t xSize) { return calloc(xNum, xSize); }
void  vPortFree(void *pv)                     { free(pv); }

/* --- time --- */
uint64_t freertos_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint32_t freertos_ticks_to_wait_ms(uint32_t ticks) {
    if (ticks == (uint32_t)portMAX_DELAY) return FREERTOS_WAIT_FOREVER;
    return (uint32_t)pdTICKS_TO_MS(ticks);
}

int freertos_cond_timedwait_ms(pthread_cond_t *cv, pthread_mutex_t *mtx, uint32_t ms) {
    if (ms == FREERTOS_WAIT_FOREVER) return pthread_cond_wait(cv, mtx);
#ifdef __APPLE__
    /* Relative wait — immune to wall-clock changes, and the default macOS cond
     * is CLOCK_REALTIME-based so an abstime form would be exposed to them. */
    struct timespec rel;
    rel.tv_sec  = ms / 1000;
    rel.tv_nsec = (long)(ms % 1000) * 1000000L;
    return pthread_cond_timedwait_relative_np(cv, mtx, &rel);
#else
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec  += ms / 1000;
    abs.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec += 1; abs.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cv, mtx, &abs);
#endif
}

/* --- ticks --- */
static uint64_t g_tick_base_ms;
static void tick_base_init(void) { if (!g_tick_base_ms) g_tick_base_ms = freertos_now_ms(); }

TickType_t xTaskGetTickCount(void) {
    tick_base_init();
    return (TickType_t)pdMS_TO_TICKS(freertos_now_ms() - g_tick_base_ms);
}
TickType_t xTaskGetTickCountFromISR(void) { return xTaskGetTickCount(); }

/* --- critical section: a single global recursive mutex (the "big kernel lock").
 *     Restores single-at-a-time semantics for code inside critical sections; it
 *     does NOT serialize ordinary parallel task code (see portmacro.h). --- */
static pthread_mutex_t g_critical = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  g_critical_once = PTHREAD_ONCE_INIT;

static void critical_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_critical, &attr);
    pthread_mutexattr_destroy(&attr);
}

void vPortEnterCritical(void) {
    pthread_once(&g_critical_once, critical_init);
    pthread_mutex_lock(&g_critical);
}
void vPortExitCritical(void) { pthread_mutex_unlock(&g_critical); }

void vTaskSuspendAll(void)        { vPortEnterCritical(); }
BaseType_t xTaskResumeAll(void)   { vPortExitCritical(); return pdFALSE; }

/* --- scheduler --- */
BaseType_t xTaskGetSchedulerState(void) { return taskSCHEDULER_RUNNING; }

void vTaskStartScheduler(void) {
    /* Unnecessary on the host: tasks are pthreads run by the OS scheduler. Block
     * forever to honour the "never returns" contract if anything calls it. */
    for (;;) pause();
}
