#pragma once

// Pure interaction model for the Edit-surface timeline: timestamp formatting,
// trim-handle clamping, and marker retiming for trimmed exports. No widget
// dependencies — everything here is deterministic and unit-testable.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <QString>

#include "RecordingMarker.h"

namespace exosnap {

// Minimum distance between the trim-in and trim-out handles. Prevents the
// handles from crossing (or producing a zero-length clip) while dragging.
inline constexpr qint64 kMinTrimGapMs = 100;

// Millisecond-precision timestamp for the drag feedback label above a
// timeline handle / the playhead: "MM:SS.mmm", with an hour field
// ("HH:MM:SS.mmm") only when the full recording is one hour or longer.
// All fields are two-digit.
inline QString FormatTimelineTimestamp(qint64 position_ms, qint64 total_ms) {
    position_ms = std::max<qint64>(position_ms, 0);
    const qint64 hours = position_ms / 3600000;
    const qint64 minutes = (position_ms / 60000) % 60;
    const qint64 seconds = (position_ms / 1000) % 60;
    const qint64 millis = position_ms % 1000;
    const bool with_hours = total_ms >= 3600000;
    if (with_hours) {
        return QStringLiteral("%1:%2:%3.%4")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'))
            .arg(millis, 3, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2.%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(millis, 3, 10, QLatin1Char('0'));
}

// Second-precision clock for the static labels under the timeline:
// "MM:SS", or "HH:MM:SS" when the full recording is one hour or longer.
inline QString FormatTimelineClock(qint64 position_ms, qint64 total_ms) {
    position_ms = std::max<qint64>(position_ms, 0);
    const qint64 hours = position_ms / 3600000;
    const qint64 minutes = (position_ms / 60000) % 60;
    const qint64 seconds = (position_ms / 1000) % 60;
    if (total_ms >= 3600000) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
}

// Clamp a dragged trim-in position: within [0, out - kMinTrimGapMs].
inline qint64 ClampTrimStartMs(qint64 requested_ms, qint64 trim_end_ms) {
    return std::clamp<qint64>(requested_ms, 0, std::max<qint64>(trim_end_ms - kMinTrimGapMs, 0));
}

// Clamp a dragged trim-out position: within [in + kMinTrimGapMs, duration].
inline qint64 ClampTrimEndMs(qint64 requested_ms, qint64 trim_start_ms, qint64 duration_ms) {
    return std::clamp<qint64>(requested_ms, std::min<qint64>(trim_start_ms + kMinTrimGapMs, duration_ms), duration_ms);
}

// Clamp a scrub/playhead position to the recording: [0, duration].
inline qint64 ClampPlayheadMs(qint64 requested_ms, qint64 duration_ms) {
    return std::clamp<qint64>(requested_ms, 0, std::max<qint64>(duration_ms, 0));
}

// Rebase session markers onto a trimmed clip. Keeps markers inside the
// half-open window [trim_start_ms, trim_end_ms) — the same boundary
// convention as PartitionSegmentMarkers (a marker exactly on the out-point
// falls outside the clip) — and shifts survivors by -trim_start_ms so their
// timestamps are relative to the new start. Markers cut away by the trim are
// dropped.
[[nodiscard]] inline std::vector<RecordingMarker> RetimeMarkersForTrim(const std::vector<RecordingMarker>& markers,
                                                                       qint64 trim_start_ms, qint64 trim_end_ms) {
    std::vector<RecordingMarker> out;
    if (trim_end_ms <= trim_start_ms)
        return out;
    for (const auto& m : markers) {
        const auto t = static_cast<qint64>(m.time_ms);
        if (t < trim_start_ms || t >= trim_end_ms)
            continue;
        RecordingMarker rebased = m;
        rebased.time_ms = static_cast<uint64_t>(t - trim_start_ms);
        out.push_back(rebased);
    }
    return out;
}

} // namespace exosnap
