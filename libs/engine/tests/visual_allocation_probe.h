#pragma once

#include <cstddef>
#include <semaphore>

namespace visual_allocations {

struct Probe {
    size_t allocations = 0;
    size_t frees = 0;
    size_t pause_size = 0;
    std::binary_semaphore* entered = nullptr;
    std::binary_semaphore* resume = nullptr;
    bool fail = false;
    size_t fail_size = 0;
    const void* pause_free = nullptr;
};

extern thread_local Probe* active;

class Scope {
  public:
    explicit Scope(Probe& probe) noexcept {
        active = &probe;
    }
    ~Scope() {
        active = nullptr;
    }
};

} // namespace visual_allocations
