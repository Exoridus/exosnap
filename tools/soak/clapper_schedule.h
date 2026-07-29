#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::soak {

struct ClapperSchedule {
    std::int64_t total_seconds = 0;
    std::vector<std::int64_t> marker_seconds;
};

bool ParsePositiveInt64(const std::string& value, std::int64_t& parsed, std::string& error);

bool BuildClapperSchedule(std::int64_t total_seconds, int marker_count, std::int64_t start_margin_seconds,
                          std::int64_t end_margin_seconds, ClapperSchedule& schedule, std::string& error);

} // namespace exosnap::soak
