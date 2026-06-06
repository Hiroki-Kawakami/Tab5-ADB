#pragma once
// Host counterpart of ESP-IDF <esp_timer.h>. Only the monotonic clock is
// implemented (the part app code commonly needs for timing); the timer-callback
// API (esp_timer_create/start) is intentionally omitted until something needs it.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Microseconds since an unspecified but fixed point (monotonic).
int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
