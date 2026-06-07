/*
 * Host FreeRTOS event groups — a bit set guarded by a mutex + condition variable.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos_internal.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct EventGroup {
    EventBits_t     bits;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
} EventGroup_t;

EventGroupHandle_t xEventGroupCreate(void) {
    EventGroup_t *eg = (EventGroup_t *)calloc(1, sizeof(EventGroup_t));
    if (!eg) return NULL;
    pthread_mutex_init(&eg->mtx, NULL);
    pthread_cond_init(&eg->cv, NULL);
    return (EventGroupHandle_t)eg;
}

void vEventGroupDelete(EventGroupHandle_t xEventGroup) {
    EventGroup_t *eg = (EventGroup_t *)xEventGroup;
    if (!eg) return;
    pthread_mutex_destroy(&eg->mtx);
    pthread_cond_destroy(&eg->cv);
    free(eg);
}

static int bits_satisfied(EventBits_t cur, EventBits_t want, BaseType_t all) {
    return all ? ((cur & want) == want) : ((cur & want) != 0);
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToWaitFor,
                                BaseType_t xClearOnExit, BaseType_t xWaitForAllBits,
                                TickType_t xTicksToWait) {
    EventGroup_t *eg = (EventGroup_t *)xEventGroup;
    if (!eg) return 0;
    uint32_t wait_ms = freertos_ticks_to_wait_ms(xTicksToWait);
    uint64_t deadline = freertos_now_ms() + (wait_ms == FREERTOS_WAIT_FOREVER ? 0 : wait_ms);

    pthread_mutex_lock(&eg->mtx);
    while (!bits_satisfied(eg->bits, uxBitsToWaitFor, xWaitForAllBits)) {
        if (wait_ms == 0) break;
        uint32_t rem = wait_ms;
        if (wait_ms != FREERTOS_WAIT_FOREVER) {
            uint64_t now = freertos_now_ms();
            if (now >= deadline) break;
            rem = (uint32_t)(deadline - now);
        }
        if (freertos_cond_timedwait_ms(&eg->cv, &eg->mtx, rem) != 0 &&
            !bits_satisfied(eg->bits, uxBitsToWaitFor, xWaitForAllBits)) {
            break;
        }
    }
    EventBits_t ret = eg->bits;
    if (xClearOnExit && bits_satisfied(eg->bits, uxBitsToWaitFor, xWaitForAllBits)) {
        eg->bits &= ~uxBitsToWaitFor;
    }
    pthread_mutex_unlock(&eg->mtx);
    return ret;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToSet) {
    EventGroup_t *eg = (EventGroup_t *)xEventGroup;
    if (!eg) return 0;
    pthread_mutex_lock(&eg->mtx);
    eg->bits |= uxBitsToSet;
    EventBits_t ret = eg->bits;
    pthread_cond_broadcast(&eg->cv);
    pthread_mutex_unlock(&eg->mtx);
    return ret;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToClear) {
    EventGroup_t *eg = (EventGroup_t *)xEventGroup;
    if (!eg) return 0;
    pthread_mutex_lock(&eg->mtx);
    EventBits_t before = eg->bits;
    eg->bits &= ~uxBitsToClear;
    pthread_mutex_unlock(&eg->mtx);
    return before;
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t xEventGroup) {
    EventGroup_t *eg = (EventGroup_t *)xEventGroup;
    if (!eg) return 0;
    pthread_mutex_lock(&eg->mtx);
    EventBits_t ret = eg->bits;
    pthread_mutex_unlock(&eg->mtx);
    return ret;
}

EventBits_t xEventGroupSync(EventGroupHandle_t xEventGroup, EventBits_t uxBitsToSet,
                            EventBits_t uxBitsToWaitFor, TickType_t xTicksToWait) {
    EventGroup_t *eg = (EventGroup_t *)xEventGroup;
    if (!eg) return 0;
    uint32_t wait_ms = freertos_ticks_to_wait_ms(xTicksToWait);
    uint64_t deadline = freertos_now_ms() + (wait_ms == FREERTOS_WAIT_FOREVER ? 0 : wait_ms);

    pthread_mutex_lock(&eg->mtx);
    eg->bits |= uxBitsToSet;
    pthread_cond_broadcast(&eg->cv);
    while ((eg->bits & uxBitsToWaitFor) != uxBitsToWaitFor) {
        if (wait_ms == 0) break;
        uint32_t rem = wait_ms;
        if (wait_ms != FREERTOS_WAIT_FOREVER) {
            uint64_t now = freertos_now_ms();
            if (now >= deadline) break;
            rem = (uint32_t)(deadline - now);
        }
        if (freertos_cond_timedwait_ms(&eg->cv, &eg->mtx, rem) != 0 &&
            (eg->bits & uxBitsToWaitFor) != uxBitsToWaitFor) {
            break;
        }
    }
    EventBits_t ret = eg->bits;
    if ((eg->bits & uxBitsToWaitFor) == uxBitsToWaitFor) {
        eg->bits &= ~uxBitsToWaitFor;
    }
    pthread_mutex_unlock(&eg->mtx);
    return ret;
}
