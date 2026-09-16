#include <exosnap/engine/device_generation.h>

#include <atomic>

namespace exosnap::engine {

DeviceGeneration NextDeviceGeneration() noexcept {
    // Starts at 1: kNoDevice is 0 and must never be minted.
    static std::atomic<uint64_t> counter{DeviceGeneration::kNoDevice};
    return DeviceGeneration{counter.fetch_add(1, std::memory_order_relaxed) + 1};
}

} // namespace exosnap::engine
