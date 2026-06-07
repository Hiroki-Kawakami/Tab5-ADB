/*
 * Host FreeRTOS queue API — a bounded ring buffer guarded by a mutex and two
 * condition variables (not-empty / not-full), with timed waits. Semaphores
 * (semphr.h) are built on the same object. There are no ISRs on the host, so the
 * *FromISR variants forward to the blocking calls with a zero timeout.
 */
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *QueueHandle_t;

/* queue send positions */
#define queueSEND_TO_BACK  ((BaseType_t)0)
#define queueSEND_TO_FRONT ((BaseType_t)1)

QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize);
void          vQueueDelete(QueueHandle_t xQueue);

BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *pvItemToQueue,
                             TickType_t xTicksToWait, BaseType_t xCopyPosition);
BaseType_t xQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait);
BaseType_t xQueuePeek(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait);
BaseType_t xQueueReset(QueueHandle_t xQueue);

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t xQueue);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t xQueue);

#define xQueueSend(q, item, ticks)        xQueueGenericSend((q), (item), (ticks), queueSEND_TO_BACK)
#define xQueueSendToBack(q, item, ticks)  xQueueGenericSend((q), (item), (ticks), queueSEND_TO_BACK)
#define xQueueSendToFront(q, item, ticks) xQueueGenericSend((q), (item), (ticks), queueSEND_TO_FRONT)

/* FromISR: no ISR context on the host → non-blocking equivalents. */
#define xQueueSendFromISR(q, item, woken)        (((void)(woken)), xQueueGenericSend((q), (item), 0, queueSEND_TO_BACK))
#define xQueueSendToBackFromISR(q, item, woken)  (((void)(woken)), xQueueGenericSend((q), (item), 0, queueSEND_TO_BACK))
#define xQueueSendToFrontFromISR(q, item, woken) (((void)(woken)), xQueueGenericSend((q), (item), 0, queueSEND_TO_FRONT))
#define xQueueReceiveFromISR(q, buf, woken)      (((void)(woken)), xQueueReceive((q), (buf), 0))
#define xQueuePeekFromISR(q, buf)                xQueuePeek((q), (buf), 0)
#define uxQueueMessagesWaitingFromISR(q)         uxQueueMessagesWaiting(q)

#ifdef __cplusplus
}
#endif
