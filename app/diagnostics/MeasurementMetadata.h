#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace exosnap::diagnostics {

// Receipt time on the monotonic clock. Attribution is explicit: absence means
// unknown, never the primary adapter or the foreground process by inference.
struct MeasurementMetadata {
    std::chrono::steady_clock::time_point observed_at{};
    std::string source;
    std::optional<int64_t> adapter_luid;
    std::optional<uint32_t> process_id;
    [[nodiscard]] bool Fresh(std::chrono::steady_clock::time_point now,
                             std::chrono::seconds maximum_age = std::chrono::seconds(15)) const noexcept {
        return observed_at != std::chrono::steady_clock::time_point{} && now >= observed_at &&
               now - observed_at <= maximum_age;
    }
    [[nodiscard]] bool FreshForAdapter(int64_t luid, std::chrono::steady_clock::time_point now,
                                       std::chrono::seconds maximum_age = std::chrono::seconds(15)) const noexcept {
        return luid != 0 && adapter_luid == luid && Fresh(now, maximum_age);
    }
};

} // namespace exosnap::diagnostics
