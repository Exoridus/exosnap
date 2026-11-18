#include "soak_storage.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace exosnap::soak {

StorageCheck CheckStorage(const StorageWatchConfig& config, const StorageObservation& observation,
                          const std::optional<StorageCheck>& previous) {
    StorageCheck result;
    result.observation = observation;
    if (!std::isfinite(config.target_seconds) || config.target_seconds <= 0 || !std::isfinite(config.poll_seconds) ||
        config.poll_seconds <= 0 || config.output_reserve_bytes == 0 || config.system_reserve_bytes == 0 ||
        config.finalization_reserve_bytes == 0 || !std::isfinite(observation.t_s) || observation.t_s < 0) {
        result.stop_reason = "storage watch configuration or timestamp is invalid";
        return result;
    }
    if (!observation.output_free_bytes || !observation.system_free_bytes || !observation.file_bytes) {
        result.stop_reason = "storage free-space or output-size observation is unavailable";
        return result;
    }
    if (previous) {
        if (!previous->observation.file_bytes || !previous->observation.output_free_bytes ||
            !previous->observation.system_free_bytes || observation.t_s <= previous->observation.t_s ||
            *observation.file_bytes < *previous->observation.file_bytes) {
            result.stop_reason = "storage output shrank or the observation clock did not advance";
            return result;
        }
        result.growth_bytes = *observation.file_bytes - *previous->observation.file_bytes;
        const double elapsed = observation.t_s - previous->observation.t_s;
        result.peak_bytes_per_second =
            std::max(previous->peak_bytes_per_second, static_cast<double>(result.growth_bytes) / elapsed);
        result.average_bytes_per_second = static_cast<double>(*observation.file_bytes) / observation.t_s;
        const auto consumed_per_second = [elapsed](uint64_t before, uint64_t after) {
            return before > after ? static_cast<double>(before - after) / elapsed : 0.0;
        };
        result.output_consumption_peak_bytes_per_second =
            std::max({previous->output_consumption_peak_bytes_per_second, result.peak_bytes_per_second,
                      consumed_per_second(*previous->observation.output_free_bytes, *observation.output_free_bytes)});
        result.system_consumption_peak_bytes_per_second =
            std::max(previous->system_consumption_peak_bytes_per_second,
                     consumed_per_second(*previous->observation.system_free_bytes, *observation.system_free_bytes));
    }
    const double remaining = std::max(0.0, config.target_seconds - observation.t_s);
    // The next poll and a full minute of drain remain writable even after an early stop.
    const auto project = [&](double rate) {
        const long double projected =
            std::ceil(static_cast<long double>(rate) * 2.0L * (remaining + config.poll_seconds + 60.0));
        const auto maximum = std::numeric_limits<uint64_t>::max();
        return projected >= static_cast<long double>(maximum) ? maximum : static_cast<uint64_t>(projected);
    };
    if (config.output_is_system_volume) {
        result.system_consumption_peak_bytes_per_second =
            std::max(result.system_consumption_peak_bytes_per_second, result.output_consumption_peak_bytes_per_second);
    }
    result.projected_remaining_bytes = project(result.output_consumption_peak_bytes_per_second);
    result.system_projected_remaining_bytes = project(result.system_consumption_peak_bytes_per_second);
    const auto enough = [&](uint64_t free, uint64_t reserve, uint64_t writes) {
        return free > reserve && free - reserve > config.finalization_reserve_bytes &&
               free - reserve - config.finalization_reserve_bytes > writes;
    };
    if (!enough(*observation.output_free_bytes, config.output_reserve_bytes, result.projected_remaining_bytes))
        result.stop_reason = "storage projected output writes would consume the reserve or finalization allowance";
    if (!enough(*observation.system_free_bytes, config.system_reserve_bytes, result.system_projected_remaining_bytes))
        result.stop_reason =
            "storage projected system-volume writes would consume the reserve or finalization allowance";
    return result;
}

} // namespace exosnap::soak
