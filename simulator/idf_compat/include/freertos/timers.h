/*
 * Host FreeRTOS software-timer API. A single daemon pthread services all timers
 * (created lazily on the first timer), mirroring the FreeRTOS timer task. One-
 * shot and auto-reload are supported. Timer-command timeouts are accepted and
 * ignored (the command list is lock-protected, never full on the host).
 */
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t xTimer);

TimerHandle_t xTimerCreate(const char *pcTimerName, TickType_t xTimerPeriod,
                           BaseType_t xAutoReload, void *pvTimerID,
                           TimerCallbackFunction_t pxCallbackFunction);
BaseType_t    xTimerStart(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t    xTimerStop(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t    xTimerChangePeriod(TimerHandle_t xTimer, TickType_t xNewPeriod, TickType_t xTicksToWait);
BaseType_t    xTimerReset(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t    xTimerDelete(TimerHandle_t xTimer, TickType_t xTicksToWait);
BaseType_t    xTimerIsTimerActive(TimerHandle_t xTimer);
void         *pvTimerGetTimerID(TimerHandle_t xTimer);
void          vTimerSetTimerID(TimerHandle_t xTimer, void *pvNewID);
const char   *pcTimerGetName(TimerHandle_t xTimer);

#define xTimerStartFromISR(t, woken)  (((void)(woken)), xTimerStart((t), 0))
#define xTimerStopFromISR(t, woken)   (((void)(woken)), xTimerStop((t), 0))
#define xTimerResetFromISR(t, woken)  (((void)(woken)), xTimerReset((t), 0))

#ifdef __cplusplus
}
#endif
