#include "esp_heap_caps.h"
#include <stdlib.h>

void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(n, size);
}

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps) {
    (void)caps;
    return realloc(ptr, size);
}

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
    (void)caps;
    void *p = NULL;
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;  // freeable with heap_caps_free()/free()
}

void heap_caps_free(void *ptr) {
    free(ptr);
}

size_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return (size_t)256 * 1024 * 1024;  // host heap always fits
}
