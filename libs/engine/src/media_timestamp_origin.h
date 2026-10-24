#pragma once

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/common.h>
#include <libavutil/mathematics.h>
}

#include <cstdint>

namespace exosnap::engine {
inline int64_t SourceRelativeTimeUs(int64_t timestamp_us, int64_t origin_us) noexcept {
    return av_sat_sub64(timestamp_us, origin_us);
}
inline int64_t SourceTimestampUs(int64_t relative_us, int64_t origin_us) noexcept {
    return av_sat_add64(relative_us, origin_us);
}
inline int64_t SourceRelativeFrameTimeUs(int64_t timestamp, AVRational time_base, int64_t origin_us) noexcept {
    if (timestamp == AV_NOPTS_VALUE)
        return 0;
    return SourceRelativeTimeUs(av_rescale_q(timestamp, time_base, AVRational{1, AV_TIME_BASE}), origin_us);
}
} // namespace exosnap::engine
