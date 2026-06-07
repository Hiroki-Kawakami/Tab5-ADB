/*
 * Host FreeRTOS task API. A task is a detached pthread; priorities are stored
 * but not enforced (host OS scheduling, best-effort). vTaskDelete supports the
 * common self-delete (NULL or own handle) robustly; deleting another task is
 * best-effort (pthread_cancel). Direct-to-task notifications are supported.
 */
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

/* Scheduler state — the host OS scheduler is always running. */
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING     1
#define taskSCHEDULER_SUSPENDED   2

/* Notification actions (eNotifyAction). */
typedef enum {
    eNoAction = 0,
    eSetBits,
    eIncrement,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite
} eNotifyAction;

BaseType_t xTaskCreate(TaskFunction_t pxTaskCode, const char *pcName,
                       const configSTACK_DEPTH_TYPE usStackDepth, void *pvParameters,
                       UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask);
/* ESP-IDF extension; the core id is ignored on the host. */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode, const char *pcName,
                                   const configSTACK_DEPTH_TYPE usStackDepth,
                                   void *pvParameters, UBaseType_t uxPriority,
                                   TaskHandle_t *pxCreatedTask, BaseType_t xCoreID);

void          vTaskDelete(TaskHandle_t xTask);
void          vTaskDelay(const TickType_t xTicksToDelay);
BaseType_t    xTaskDelayUntil(TickType_t *pxPreviousWakeTime, const TickType_t xTimeIncrement);
void          vTaskDelayUntil(TickType_t *pxPreviousWakeTime, const TickType_t xTimeIncrement);
TickType_t    xTaskGetTickCount(void);
TickType_t    xTaskGetTickCountFromISR(void);
TaskHandle_t  xTaskGetCurrentTaskHandle(void);
const char   *pcTaskGetName(TaskHandle_t xTask);
UBaseType_t   uxTaskPriorityGet(TaskHandle_t xTask);
void          vTaskPrioritySet(TaskHandle_t xTask, UBaseType_t uxNewPriority);
BaseType_t    xTaskGetSchedulerState(void);

/* vTaskStartScheduler() is unnecessary on the host (the OS is the scheduler).
 * Provided only for source compatibility; it blocks forever like the real one
 * so a stray call cannot fall through and exit the program. */
void vTaskStartScheduler(void);

/* Cooperative-scheduling stubs: map to the global lock (see portmacro.h). */
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);

/* --- direct-to-task notifications --- */
BaseType_t xTaskGenericNotify(TaskHandle_t xTask, UBaseType_t uxIndex, uint32_t ulValue,
                              eNotifyAction eAction, uint32_t *pulPreviousNotificationValue);
BaseType_t xTaskGenericNotifyWait(UBaseType_t uxIndex, uint32_t ulBitsToClearOnEntry,
                                  uint32_t ulBitsToClearOnExit, uint32_t *pulNotificationValue,
                                  TickType_t xTicksToWait);
uint32_t   ulTaskGenericNotifyTake(UBaseType_t uxIndex, BaseType_t xClearCountOnExit,
                                   TickType_t xTicksToWait);

#define tskNO_AFFINITY      ((BaseType_t)0x7FFFFFFF)
#define tskDEFAULT_INDEX    0

#define xTaskNotify(task, val, act)          xTaskGenericNotify((task), tskDEFAULT_INDEX, (val), (act), NULL)
#define xTaskNotifyGive(task)                xTaskGenericNotify((task), tskDEFAULT_INDEX, 0, eIncrement, NULL)
#define vTaskNotifyGiveFromISR(task, woken)  do { (void)(woken); xTaskGenericNotify((task), tskDEFAULT_INDEX, 0, eIncrement, NULL); } while (0)
#define xTaskNotifyFromISR(task, val, act, woken) (((void)(woken)), xTaskGenericNotify((task), tskDEFAULT_INDEX, (val), (act), NULL))
#define ulTaskNotifyTake(clr, ticks)         ulTaskGenericNotifyTake(tskDEFAULT_INDEX, (clr), (ticks))
#define xTaskNotifyWait(clrEntry, clrExit, pval, ticks) \
        xTaskGenericNotifyWait(tskDEFAULT_INDEX, (clrEntry), (clrExit), (pval), (ticks))

#ifdef __cplusplus
}
#endif
