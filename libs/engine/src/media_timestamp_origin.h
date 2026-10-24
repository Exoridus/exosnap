#pragma once

extern "C" {
#include <libavutil/common.h>
}

#include <cstdint>

namespace exosnap::engine {
inline int64_t SourceRelativeTimeUs(int64_t timestamp_us, int64_t origin_us) noexcept {
    return av_sat_sub64(timestamp_us, origin_us);
}
inline int64_t SourceTimestampUs(int64_t relative_us, int64_t origin_us) noexcept {
    return av_sat_add64(relative_us, origin_us);
}
} // namespace exosnap::engine
