#pragma once
// Host counterpart of ESP-IDF <esp_heap_caps.h>: the capability-tagged allocator.
// On the host every allocation is plain malloc — the MALLOC_CAP_* flags are
// accepted and ignored (there is no SPIRAM/DMA/internal distinction off-device).
// The MALLOC_CAP_* bit values mirror ESP-IDF so app code can OR/test them.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_EXEC        (1 << 0)
#define MALLOC_CAP_32BIT       (1 << 1)
#define MALLOC_CAP_8BIT        (1 << 2)
#define MALLOC_CAP_DMA         (1 << 3)
#define MALLOC_CAP_SPIRAM      (1 << 10)
#define MALLOC_CAP_INTERNAL    (1 << 11)
#define MALLOC_CAP_DEFAULT     (1 << 12)
#define MALLOC_CAP_IRAM_8BIT   (1 << 13)
#define MALLOC_CAP_RETENTION   (1 << 14)
#define MALLOC_CAP_RTCRAM      (1 << 15)
#define MALLOC_CAP_TCM         (1 << 16)
#define MALLOC_CAP_CACHE_ALIGNED (1 << 19)
#define MALLOC_CAP_INVALID     (1 << 31)

void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void  heap_caps_free(void *ptr);

// Largest allocatable contiguous block for the given caps. On the host there is
// no fragmentation model, so report a large value (the host heap always fits).
size_t heap_caps_get_largest_free_block(uint32_t caps);

#ifdef __cplusplus
}
#endif
