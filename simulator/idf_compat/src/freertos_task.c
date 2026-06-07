/*
 * Host FreeRTOS tasks + direct-to-task notifications. A task is a detached
 * pthread with a generous default stack (no embedded stack sizing → none of the
 * page-rounding hazards the vendored Posix port had). Priorities are stored but
 * not enforced. Self-delete (vTaskDelete(NULL)) is fully supported; deleting
 * another task is best-effort (pthread_cancel) and intentionally leaks its TCB
 * to avoid a use-after-free race with the still-running thread.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct TCB {
    pthread_t       thread;
    char            name[24];
    TaskFunction_t  code;
    void           *param;
    UBaseType_t     priority;
    /* notification state (single index) */
    pthread_mutex_t ntf_mtx;
    pthread_cond_t  ntf_cv;
    uint32_t        ntf_value;
    int             ntf_pending;
} TCB_t;

static pthread_key_t  g_current_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static void make_key(void) { pthread_key_create(&g_current_key, NULL); }

static TCB_t *current_tcb(void) {
    pthread_once(&g_key_once, make_key);
    return (TCB_t *)pthread_getspecific(g_current_key);
}

static void *trampoline(void *arg) {
    TCB_t *t = (TCB_t *)arg;
    pthread_once(&g_key_once, make_key);
    pthread_setspecific(g_current_key, t);
    t->code(t->param);
    /* FreeRTOS forbids returning from a task body; on the host, self-delete. */
    vTaskDelete(NULL);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t pxTaskCode, const char *pcName,
                       const configSTACK_DEPTH_TYPE usStackDepth, void *pvParameters,
                       UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask) {
    (void)usStackDepth; /* host pthread default stack is plenty */
    TCB_t *t = (TCB_t *)calloc(1, sizeof(TCB_t));
    if (!t) return pdFAIL;
    if (pcName) { strncpy(t->name, pcName, sizeof(t->name) - 1); }
    t->code = pxTaskCode;
    t->param = pvParameters;
    t->priority = uxPriority;
    pthread_mutex_init(&t->ntf_mtx, NULL);
    pthread_cond_init(&t->ntf_cv, NULL);

    if (pthread_create(&t->thread, NULL, trampoline, t) != 0) {
        pthread_mutex_destroy(&t->ntf_mtx);
        pthread_cond_destroy(&t->ntf_cv);
        free(t);
        return pdFAIL;
    }
    pthread_detach(t->thread);
    if (pxCreatedTask) *pxCreatedTask = (TaskHandle_t)t;
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode, const char *pcName,
                                   const configSTACK_DEPTH_TYPE usStackDepth,
                                   void *pvParameters, UBaseType_t uxPriority,
                                   TaskHandle_t *pxCreatedTask, BaseType_t xCoreID) {
    (void)xCoreID;
    return xTaskCreate(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask);
}

void vTaskDelete(TaskHandle_t xTask) {
    TCB_t *self = current_tcb();
    TCB_t *t = xTask ? (TCB_t *)xTask : self;
    if (!t) return;

    if (t == self) {
        pthread_mutex_destroy(&t->ntf_mtx);
        pthread_cond_destroy(&t->ntf_cv);
        free(t);
        pthread_exit(NULL);
    } else {
        /* Best-effort: cancel the thread; leak the TCB to avoid racing the
         * still-running thread to free it. Self-delete is the common path. */
        pthread_cancel(t->thread);
    }
}

void vTaskDelay(const TickType_t xTicksToDelay) {
    uint32_t ms = (uint32_t)pdTICKS_TO_MS(xTicksToDelay);
    if (ms == 0) { sched_yield(); return; }
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

BaseType_t xTaskDelayUntil(TickType_t *pxPreviousWakeTime, const TickType_t xTimeIncrement) {
    TickType_t now = xTaskGetTickCount();
    TickType_t target = *pxPreviousWakeTime + xTimeIncrement;
    *pxPreviousWakeTime = target;
    if ((int32_t)(target - now) > 0) {
        vTaskDelay(target - now);
        return pdTRUE;
    }
    return pdFALSE;
}
void vTaskDelayUntil(TickType_t *pxPreviousWakeTime, const TickType_t xTimeIncrement) {
    (void)xTaskDelayUntil(pxPreviousWakeTime, xTimeIncrement);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)current_tcb(); }

const char *pcTaskGetName(TaskHandle_t xTask) {
    TCB_t *t = xTask ? (TCB_t *)xTask : current_tcb();
    return t ? t->name : "";
}

UBaseType_t uxTaskPriorityGet(TaskHandle_t xTask) {
    TCB_t *t = xTask ? (TCB_t *)xTask : current_tcb();
    return t ? t->priority : 0;
}
void vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority) {
    TCB_t *t = xTask ? (TCB_t *)xTask : current_tcb();
    if (t) t->priority = uxNewPriority; /* not enforced on the host */
}

/* --- notifications --- */
BaseType_t xTaskGenericNotify(TaskHandle_t xTask, UBaseType_t uxIndex, uint32_t ulValue,
                              eNotifyAction eAction, uint32_t *pulPreviousNotificationValue) {
    (void)uxIndex;
    TCB_t *t = (TCB_t *)xTask;
    if (!t) return pdFAIL;
    pthread_mutex_lock(&t->ntf_mtx);
    if (pulPreviousNotificationValue) *pulPreviousNotificationValue = t->ntf_value;
    BaseType_t ret = pdPASS;
    switch (eAction) {
        case eSetBits:                   t->ntf_value |= ulValue; break;
        case eIncrement:                 t->ntf_value += 1; break;
        case eSetValueWithOverwrite:     t->ntf_value = ulValue; break;
        case eSetValueWithoutOverwrite:
            if (!t->ntf_pending) t->ntf_value = ulValue; else ret = pdFAIL;
            break;
        case eNoAction:                  break;
    }
    t->ntf_pending = 1;
    pthread_cond_signal(&t->ntf_cv);
    pthread_mutex_unlock(&t->ntf_mtx);
    return ret;
}

uint32_t ulTaskGenericNotifyTake(UBaseType_t uxIndex, BaseType_t xClearCountOnExit,
                                 TickType_t xTicksToWait) {
    (void)uxIndex;
    TCB_t *t = current_tcb();
    if (!t) return 0;
    uint32_t wait_ms = freertos_ticks_to_wait_ms(xTicksToWait);
    uint64_t deadline = freertos_now_ms() + (wait_ms == FREERTOS_WAIT_FOREVER ? 0 : wait_ms);

    pthread_mutex_lock(&t->ntf_mtx);
    while (t->ntf_value == 0) {
        uint32_t rem = wait_ms;
        if (wait_ms != FREERTOS_WAIT_FOREVER) {
            uint64_t now = freertos_now_ms();
            if (now >= deadline) break;
            rem = (uint32_t)(deadline - now);
        }
        if (freertos_cond_timedwait_ms(&t->ntf_cv, &t->ntf_mtx, rem) != 0) break;
    }
    uint32_t ret = t->ntf_value;
    if (xClearCountOnExit) t->ntf_value = 0; else if (t->ntf_value) t->ntf_value--;
    t->ntf_pending = (t->ntf_value != 0);
    pthread_mutex_unlock(&t->ntf_mtx);
    return ret;
}

BaseType_t xTaskGenericNotifyWait(UBaseType_t uxIndex, uint32_t ulBitsToClearOnEntry,
                                  uint32_t ulBitsToClearOnExit, uint32_t *pulNotificationValue,
                                  TickType_t xTicksToWait) {
    (void)uxIndex;
    TCB_t *t = current_tcb();
    if (!t) return pdFALSE;
    uint32_t wait_ms = freertos_ticks_to_wait_ms(xTicksToWait);
    uint64_t deadline = freertos_now_ms() + (wait_ms == FREERTOS_WAIT_FOREVER ? 0 : wait_ms);

    pthread_mutex_lock(&t->ntf_mtx);
    if (!t->ntf_pending) t->ntf_value &= ~ulBitsToClearOnEntry;
    BaseType_t got = pdTRUE;
    while (!t->ntf_pending) {
        uint32_t rem = wait_ms;
        if (wait_ms != FREERTOS_WAIT_FOREVER) {
            uint64_t now = freertos_now_ms();
            if (now >= deadline) { got = pdFALSE; break; }
            rem = (uint32_t)(deadline - now);
        }
        if (freertos_cond_timedwait_ms(&t->ntf_cv, &t->ntf_mtx, rem) != 0) {
            got = t->ntf_pending ? pdTRUE : pdFALSE;
            break;
        }
    }
    if (pulNotificationValue) *pulNotificationValue = t->ntf_value;
    if (got) { t->ntf_value &= ~ulBitsToClearOnExit; t->ntf_pending = 0; }
    pthread_mutex_unlock(&t->ntf_mtx);
    return got;
}
