#include "clapper_schedule.h"

#include <charconv>
#include <limits>

namespace exosnap::soak {

bool ParsePositiveInt64(const std::string& value, std::int64_t& parsed, std::string& error) {
    if (value.empty()) {
        error = "missing positive integer value";
        return false;
    }

    std::int64_t candidate = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [next, ec] = std::from_chars(begin, end, candidate);
    if (ec == std::errc::result_out_of_range) {
        error = "integer value is out of range: " + value;
        return false;
    }
    if (ec != std::errc{} || next != end || candidate <= 0) {
        error = "expected a positive integer, got: " + value;
        return false;
    }

    parsed = candidate;
    return true;
}

bool BuildClapperSchedule(std::int64_t total_seconds, int marker_count, std::int64_t start_margin_seconds,
                          std::int64_t end_margin_seconds, ClapperSchedule& schedule, std::string& error) {
    if (total_seconds <= 0) {
        error = "clapper duration must be positive";
        return false;
    }
    if (marker_count != 2 && marker_count != 3) {
        error = "clapper marker count must be 2 or 3";
        return false;
    }
    if (start_margin_seconds < 0 || end_margin_seconds < 0) {
        error = "clapper margins cannot be negative";
        return false;
    }
    if (start_margin_seconds > std::numeric_limits<std::int64_t>::max() - end_margin_seconds ||
        start_margin_seconds + end_margin_seconds >= total_seconds) {
        error = "clapper margins leave no measurable marker span";
        return false;
    }

    const std::int64_t first = start_margin_seconds;
    const std::int64_t last = total_seconds - end_margin_seconds;
    std::vector<std::int64_t> markers = {first};
    if (marker_count == 3)
        markers.push_back(total_seconds / 2);
    markers.push_back(last);

    for (std::size_t i = 1; i < markers.size(); ++i) {
        if (markers[i] <= markers[i - 1]) {
            error = "clapper duration and margins do not produce strictly ordered markers";
            return false;
        }
    }

    schedule.total_seconds = total_seconds;
    schedule.marker_seconds = std::move(markers);
    return true;
}

} // namespace exosnap::soak
