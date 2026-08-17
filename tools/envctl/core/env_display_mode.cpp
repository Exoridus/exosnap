#include "env_display_mode.h"

#include <algorithm>
#include <cstdio>
#include <tuple>

namespace exosnap::envctl {
namespace {

auto AsTuple(const DisplayModeFacts& mode) {
    return std::tie(mode.width, mode.height, mode.refresh_hz, mode.bits_per_pixel, mode.orientation_degrees);
}

} // namespace

bool operator==(const DisplayModeFacts& lhs, const DisplayModeFacts& rhs) {
    return AsTuple(lhs) == AsTuple(rhs);
}

bool operator!=(const DisplayModeFacts& lhs, const DisplayModeFacts& rhs) {
    return !(lhs == rhs);
}

std::string FormatModeFacts(const DisplayModeFacts& mode) {
    char buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer), "%lux%lu@%lux%lu/%lu", mode.width, mode.height, mode.refresh_hz,
                  mode.bits_per_pixel, mode.orientation_degrees);
    return std::string(buffer);
}

bool SameGeometry(const DisplayModeFacts& lhs, const DisplayModeFacts& rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height && lhs.bits_per_pixel == rhs.bits_per_pixel &&
           lhs.orientation_degrees == rhs.orientation_degrees;
}

std::vector<DisplayModeFacts> RefreshRateCandidates(const DisplayModeFacts& current,
                                                    const std::vector<DisplayModeFacts>& enumerated) {
    std::vector<DisplayModeFacts> candidates;
    for (const auto& mode : enumerated) {
        if (!SameGeometry(current, mode)) {
            continue;
        }
        candidates.push_back(mode);
    }

    // Sorting and de-duplication are the only processing applied, and neither
    // touches a value: EnumDisplaySettingsEx repeats one mode once per
    // dmDisplayFlags variant, which would otherwise fill the list with rows a
    // caller cannot tell apart. The refresh rates themselves stay verbatim.
    std::sort(candidates.begin(), candidates.end(),
              [](const DisplayModeFacts& lhs, const DisplayModeFacts& rhs) { return AsTuple(lhs) < AsTuple(rhs); });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

} // namespace exosnap::envctl
