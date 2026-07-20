#pragma once

#include <cstddef>
#include <cstdlib>

#include "esp_heap_caps.h"

template <class T>
struct PsramAllocator {
    using value_type = T;

    PsramAllocator() = default;

    template <class U>
    PsramAllocator(const PsramAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
        if (!p) abort();  // Device builds cannot throw std::bad_alloc.
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
};

template <class A, class B>
bool operator==(const PsramAllocator<A>&, const PsramAllocator<B>&) noexcept {
    return true;
}

template <class A, class B>
bool operator!=(const PsramAllocator<A>&, const PsramAllocator<B>&) noexcept {
    return false;
}
