/*
 * Host FreeRTOS queues + semaphores. A queue is a ring buffer guarded by a mutex
 * and two condition variables. Semaphores reuse the same object with item size 0,
 * so the "message count" is the token count; mutexes track owner + recursion.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct Queue {
    uint8_t        *storage;
    size_t          item_size;
    size_t          length;     /* capacity in items */
    size_t          count;      /* items currently queued */
    size_t          head;       /* read index */
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    /* mutex-semaphore bookkeeping (mutex_kind: 0 none, 1 mutex, 2 recursive) */
    int             mutex_kind;
    pthread_t       owner;
    int             recursion;
} Queue_t;

QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize) {
    Queue_t *q = (Queue_t *)calloc(1, sizeof(Queue_t));
    if (!q) return NULL;
    q->item_size = uxItemSize;
    q->length = uxQueueLength ? uxQueueLength : 1;
    if (uxItemSize) {
        q->storage = (uint8_t *)malloc(q->length * uxItemSize);
        if (!q->storage) { free(q); return NULL; }
    }
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return (QueueHandle_t)q;
}

void vQueueDelete(QueueHandle_t xQueue) {
    Queue_t *q = (Queue_t *)xQueue;
    if (!q) return;
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->storage);
    free(q);
}

static BaseType_t wait_until(Queue_t *q, pthread_cond_t *cv, uint32_t wait_ms,
                             int (*ready)(Queue_t *)) {
    uint64_t deadline = freertos_now_ms() + (wait_ms == FREERTOS_WAIT_FOREVER ? 0 : wait_ms);
    while (!ready(q)) {
        if (wait_ms == 0) return pdFALSE;
        uint32_t rem = wait_ms;
        if (wait_ms != FREERTOS_WAIT_FOREVER) {
            uint64_t now = freertos_now_ms();
            if (now >= deadline) return pdFALSE;
            rem = (uint32_t)(deadline - now);
        }
        if (freertos_cond_timedwait_ms(cv, &q->mtx, rem) != 0 && !ready(q)) return pdFALSE;
    }
    return pdTRUE;
}

static int has_space(Queue_t *q) { return q->count < q->length; }
static int has_item(Queue_t *q)  { return q->count > 0; }

BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *pvItemToQueue,
                             TickType_t xTicksToWait, BaseType_t xCopyPosition) {
    Queue_t *q = (Queue_t *)xQueue;
    if (!q) return pdFAIL;
    uint32_t wait_ms = freertos_ticks_to_wait_ms(xTicksToWait);
    pthread_mutex_lock(&q->mtx);
    if (!wait_until(q, &q->not_full, wait_ms, has_space)) {
        pthread_mutex_unlock(&q->mtx);
        return errQUEUE_FULL;
    }
    if (q->item_size) {
        if (xCopyPosition == queueSEND_TO_FRONT) {
            q->head = (q->head + q->length - 1) % q->length;
            memcpy(q->storage + q->head * q->item_size, pvItemToQueue, q->item_size);
        } else {
            size_t tail = (q->head + q->count) % q->length;
            memcpy(q->storage + tail * q->item_size, pvItemToQueue, q->item_size);
        }
    }
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
    return pdPASS;
}

static BaseType_t queue_recv(Queue_t *q, void *pvBuffer, uint32_t wait_ms, int peek) {
    pthread_mutex_lock(&q->mtx);
    if (!wait_until(q, &q->not_empty, wait_ms, has_item)) {
        pthread_mutex_unlock(&q->mtx);
        return errQUEUE_EMPTY;
    }
    if (q->item_size && pvBuffer) {
        memcpy(pvBuffer, q->storage + q->head * q->item_size, q->item_size);
    }
    if (!peek) {
        q->head = (q->head + 1) % q->length;
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->mtx);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait) {
    Queue_t *q = (Queue_t *)xQueue;
    if (!q) return pdFAIL;
    return queue_recv(q, pvBuffer, freertos_ticks_to_wait_ms(xTicksToWait), 0);
}
BaseType_t xQueuePeek(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait) {
    Queue_t *q = (Queue_t *)xQueue;
    if (!q) return pdFAIL;
    return queue_recv(q, pvBuffer, freertos_ticks_to_wait_ms(xTicksToWait), 1);
}

BaseType_t xQueueReset(QueueHandle_t xQueue) {
    Queue_t *q = (Queue_t *)xQueue;
    if (!q) return pdFAIL;
    pthread_mutex_lock(&q->mtx);
    q->count = 0; q->head = 0;
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return pdPASS;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t xQueue) {
    Queue_t *q = (Queue_t *)xQueue;
    if (!q) return 0;
    pthread_mutex_lock(&q->mtx);
    UBaseType_t n = (UBaseType_t)q->count;
    pthread_mutex_unlock(&q->mtx);
    return n;
}
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t xQueue) {
    Queue_t *q = (Queue_t *)xQueue;
    if (!q) return 0;
    pthread_mutex_lock(&q->mtx);
    UBaseType_t n = (UBaseType_t)(q->length - q->count);
    pthread_mutex_unlock(&q->mtx);
    return n;
}

/* --- semaphores (zero-item queue; count == token count) --- */
SemaphoreHandle_t freertos_sem_create(UBaseType_t uxMaxCount, UBaseType_t uxInitialCount,
                                      int mutex_kind) {
    Queue_t *q = (Queue_t *)xQueueCreate(uxMaxCount, 0);
    if (!q) return NULL;
    q->count = uxInitialCount;       /* mutexes start available (count 1) */
    q->mutex_kind = mutex_kind;
    return (SemaphoreHandle_t)q;
}

BaseType_t freertos_sem_take(SemaphoreHandle_t s, TickType_t xTicksToWait) {
    Queue_t *q = (Queue_t *)s;
    if (!q) return pdFAIL;
    BaseType_t r = queue_recv(q, NULL, freertos_ticks_to_wait_ms(xTicksToWait), 0);
    if (r == pdPASS && q->mutex_kind) { q->owner = pthread_self(); q->recursion = 1; }
    return r;
}

BaseType_t freertos_sem_give(SemaphoreHandle_t s) {
    Queue_t *q = (Queue_t *)s;
    if (!q) return pdFAIL;
    if (q->mutex_kind) { q->owner = 0; q->recursion = 0; }
    return xQueueGenericSend(s, NULL, 0, queueSEND_TO_BACK);
}

BaseType_t freertos_sem_take_recursive(SemaphoreHandle_t s, TickType_t xTicksToWait) {
    Queue_t *q = (Queue_t *)s;
    if (!q) return pdFAIL;
    if (q->mutex_kind == 2 && q->recursion > 0 && pthread_equal(q->owner, pthread_self())) {
        q->recursion++;
        return pdPASS;
    }
    return freertos_sem_take(s, xTicksToWait);
}

BaseType_t freertos_sem_give_recursive(SemaphoreHandle_t s) {
    Queue_t *q = (Queue_t *)s;
    if (!q) return pdFAIL;
    if (q->mutex_kind == 2 && q->recursion > 1 && pthread_equal(q->owner, pthread_self())) {
        q->recursion--;
        return pdPASS;
    }
    return freertos_sem_give(s);
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s) {
    return uxQueueMessagesWaiting(s);
}
