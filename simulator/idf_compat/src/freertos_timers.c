/*
 * Host FreeRTOS software timers. A single daemon pthread (the host analogue of
 * the FreeRTOS timer task, started lazily on first use) sleeps until the nearest
 * expiry and fires callbacks. Callbacks run with the timer lock released, so a
 * callback may safely call the timer API. Command timeouts are ignored.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct Timer {
    char                    name[24];
    uint32_t                period_ms;
    int                     auto_reload;
    void                   *id;
    TimerCallbackFunction_t cb;
    int                     active;
    int                     firing;          /* callback in progress (lock released) */
    int                     pending_delete;
    uint64_t                expiry_ms;
    struct Timer           *next;
} Timer_t;

static Timer_t        *g_timers;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv  = PTHREAD_COND_INITIALIZER;
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;

static void unlink_timer(Timer_t *t) {
    Timer_t **pp = &g_timers;
    while (*pp) { if (*pp == t) { *pp = t->next; return; } pp = &(*pp)->next; }
}

static void *daemon_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mtx);
    for (;;) {
        uint64_t now = freertos_now_ms();
        Timer_t *due = NULL;
        uint64_t earliest = 0;
        int have_active = 0;
        for (Timer_t *t = g_timers; t; t = t->next) {
            if (!t->active) continue;
            if (t->expiry_ms <= now) { due = t; break; }
            if (!have_active || t->expiry_ms < earliest) { earliest = t->expiry_ms; have_active = 1; }
        }
        if (due) {
            due->active = due->auto_reload ? 1 : 0;
            if (due->auto_reload) due->expiry_ms = now + due->period_ms;
            TimerCallbackFunction_t cb = due->cb;
            due->firing = 1;
            pthread_mutex_unlock(&g_mtx);
            if (cb) cb((TimerHandle_t)due);
            pthread_mutex_lock(&g_mtx);
            due->firing = 0;
            if (due->pending_delete) { unlink_timer(due); free(due); }
            continue;
        }
        if (have_active) {
            freertos_cond_timedwait_ms(&g_cv, &g_mtx, (uint32_t)(earliest - now));
        } else {
            pthread_cond_wait(&g_cv, &g_mtx);
        }
    }
    return NULL;
}

static void daemon_start(void) {
    pthread_t tid;
    pthread_create(&tid, NULL, daemon_main, NULL);
    pthread_detach(tid);
}

TimerHandle_t xTimerCreate(const char *pcTimerName, TickType_t xTimerPeriod,
                           BaseType_t xAutoReload, void *pvTimerID,
                           TimerCallbackFunction_t pxCallbackFunction) {
    pthread_once(&g_once, daemon_start);
    Timer_t *t = (Timer_t *)calloc(1, sizeof(Timer_t));
    if (!t) return NULL;
    if (pcTimerName) strncpy(t->name, pcTimerName, sizeof(t->name) - 1);
    t->period_ms = (uint32_t)pdTICKS_TO_MS(xTimerPeriod);
    t->auto_reload = (xAutoReload != pdFALSE);
    t->id = pvTimerID;
    t->cb = pxCallbackFunction;
    pthread_mutex_lock(&g_mtx);
    t->next = g_timers;
    g_timers = t;
    pthread_mutex_unlock(&g_mtx);
    return (TimerHandle_t)t;
}

static BaseType_t arm(Timer_t *t, uint32_t new_period_ms) {
    pthread_mutex_lock(&g_mtx);
    if (new_period_ms) t->period_ms = new_period_ms;
    t->active = 1;
    t->expiry_ms = freertos_now_ms() + t->period_ms;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mtx);
    return pdPASS;
}

BaseType_t xTimerStart(TimerHandle_t xTimer, TickType_t xTicksToWait) {
    (void)xTicksToWait;
    return xTimer ? arm((Timer_t *)xTimer, 0) : pdFAIL;
}
BaseType_t xTimerReset(TimerHandle_t xTimer, TickType_t xTicksToWait) {
    (void)xTicksToWait;
    return xTimer ? arm((Timer_t *)xTimer, 0) : pdFAIL;
}
BaseType_t xTimerChangePeriod(TimerHandle_t xTimer, TickType_t xNewPeriod, TickType_t xTicksToWait) {
    (void)xTicksToWait;
    return xTimer ? arm((Timer_t *)xTimer, (uint32_t)pdTICKS_TO_MS(xNewPeriod)) : pdFAIL;
}

BaseType_t xTimerStop(TimerHandle_t xTimer, TickType_t xTicksToWait) {
    (void)xTicksToWait;
    Timer_t *t = (Timer_t *)xTimer;
    if (!t) return pdFAIL;
    pthread_mutex_lock(&g_mtx);
    t->active = 0;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mtx);
    return pdPASS;
}

BaseType_t xTimerDelete(TimerHandle_t xTimer, TickType_t xTicksToWait) {
    (void)xTicksToWait;
    Timer_t *t = (Timer_t *)xTimer;
    if (!t) return pdFAIL;
    pthread_mutex_lock(&g_mtx);
    t->active = 0;
    if (t->firing) {
        /* daemon is mid-callback on this timer; it frees on return */
        t->pending_delete = 1;
    } else {
        unlink_timer(t);
        free(t);
    }
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mtx);
    return pdPASS;
}

BaseType_t xTimerIsTimerActive(TimerHandle_t xTimer) {
    Timer_t *t = (Timer_t *)xTimer;
    if (!t) return pdFALSE;
    pthread_mutex_lock(&g_mtx);
    BaseType_t a = t->active ? pdTRUE : pdFALSE;
    pthread_mutex_unlock(&g_mtx);
    return a;
}

void *pvTimerGetTimerID(TimerHandle_t xTimer) {
    Timer_t *t = (Timer_t *)xTimer;
    return t ? t->id : NULL;
}
void vTimerSetTimerID(TimerHandle_t xTimer, void *pvNewID) {
    Timer_t *t = (Timer_t *)xTimer;
    if (t) t->id = pvNewID;
}
const char *pcTimerGetName(TimerHandle_t xTimer) {
    Timer_t *t = (Timer_t *)xTimer;
    return t ? t->name : "";
}
