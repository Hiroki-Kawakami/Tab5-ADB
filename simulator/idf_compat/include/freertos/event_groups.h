/*
 * Host FreeRTOS event-group API — a bit set guarded by a mutex + condition
 * variable. Waiters block until their bit pattern is satisfied (any/all) or the
 * timeout elapses.
 */
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void    *EventGroupHandle_t;
typedef uint32_t EventBits_t;

EventGroupHandle_t xEventGroupCreate(void);
void               vEventGroupDelete(EventGroupHandle_t xEventGroup);

EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToWaitFor,
                                BaseType_t xClearOnExit, BaseType_t xWaitForAllBits,
                                TickType_t xTicksToWait);
EventBits_t xEventGroupSetBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToSet);
EventBits_t xEventGroupClearBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToClear);
EventBits_t xEventGroupGetBits(EventGroupHandle_t xEventGroup);
EventBits_t xEventGroupSync(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToSet,
                            EventBits_t uxBitsToWaitFor, TickType_t xTicksToWait);

#define xEventGroupSetBitsFromISR(eg, bits, woken)  (((void)(woken)), xEventGroupSetBits((eg), (bits)))
#define xEventGroupClearBitsFromISR(eg, bits)       xEventGroupClearBits((eg), (bits))
#define xEventGroupGetBitsFromISR(eg)               xEventGroupGetBits((eg))

#ifdef __cplusplus
}
#endif
