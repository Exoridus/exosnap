#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace exosnap::soak {

struct StorageWatchConfig {
    double target_seconds = 0.0;
    double poll_seconds = 60.0;
    uint64_t output_reserve_bytes = 0;
    uint64_t system_reserve_bytes = 0;
    uint64_t finalization_reserve_bytes = 1024ULL * 1024 * 1024;
    bool output_is_system_volume = false;
};

struct StorageObservation {
    double t_s = 0.0;
    std::optional<uint64_t> output_free_bytes = std::nullopt;
    std::optional<uint64_t> system_free_bytes = std::nullopt;
    std::optional<uint64_t> file_bytes = std::nullopt;
};

struct StorageCheck {
    StorageObservation observation;
    uint64_t growth_bytes = 0;
    double average_bytes_per_second = 0.0;
    double peak_bytes_per_second = 0.0;
    double output_consumption_peak_bytes_per_second = 0.0;
    double system_consumption_peak_bytes_per_second = 0.0;
    uint64_t projected_remaining_bytes = 0;
    uint64_t system_projected_remaining_bytes = 0;
    std::string stop_reason;
};

// Projects remaining writes from observed file growth and volume consumption, retaining reserves on both volumes.
[[nodiscard]] StorageCheck CheckStorage(const StorageWatchConfig& config, const StorageObservation& observation,
                                        const std::optional<StorageCheck>& previous);

} // namespace exosnap::soak
