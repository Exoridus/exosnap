#include "visual_allocation_probe.h"

#include <cstdlib>
#include <new>

namespace visual_allocations {
thread_local Probe* active = nullptr;

bool MatchesAllocation(size_t actual, size_t requested) noexcept {
    return requested != 0 && actual >= requested && actual - requested <= active->allocation_overhead;
}

void* Allocate(size_t size) {
    if (active) {
        ++active->allocations;
        if (MatchesAllocation(size, active->pause_size) && active->entered && active->resume) {
            active->pause_size = 0;
            active->entered->release();
            active->resume->acquire();
        }
        if (active->fail && (active->fail_size == 0 || MatchesAllocation(size, active->fail_size))) {
            active->fail = false;
            throw std::bad_alloc();
        }
    }
    void* pointer = nullptr;
    while (!(pointer = std::malloc(size ? size : 1))) {
        const auto handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
    return pointer;
}

void Free(void* pointer) noexcept {
    if (active && pointer) {
        ++active->frees;
        if (active->pause_free == pointer && active->entered && active->resume) {
            active->pause_free = nullptr;
            active->entered->release();
            active->resume->acquire();
        }
    }
    std::free(pointer);
}
} // namespace visual_allocations

void* operator new(size_t size) {
    return visual_allocations::Allocate(size);
}
void* operator new[](size_t size) {
    return visual_allocations::Allocate(size);
}
void operator delete(void* pointer) noexcept {
    visual_allocations::Free(pointer);
}
void operator delete[](void* pointer) noexcept {
    visual_allocations::Free(pointer);
}
