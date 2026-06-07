/*
 * Internal helpers shared across the host FreeRTOS compat sources. Not part of
 * the public FreeRTOS API.
 */
#pragma once

#include <pthread.h>
#include <stdint.h>

#define FREERTOS_WAIT_FOREVER 0xffffffffUL

/* Monotonic milliseconds since process start (CLOCK_MONOTONIC). */
uint64_t freertos_now_ms(void);

/* Convert a FreeRTOS tick timeout to a millisecond timeout, mapping portMAX_DELAY
 * to FREERTOS_WAIT_FOREVER. */
uint32_t freertos_ticks_to_wait_ms(uint32_t ticks);

/* Timed condition wait. ms == FREERTOS_WAIT_FOREVER blocks indefinitely.
 * Returns 0 if signalled, ETIMEDOUT on timeout (monotonic-safe relative wait). */
int freertos_cond_timedwait_ms(pthread_cond_t *cv, pthread_mutex_t *mtx, uint32_t ms);
