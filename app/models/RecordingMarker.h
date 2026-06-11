#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace exosnap {

enum class RecordingMarkerType {
    General,
    Cut,
    Highlight,
};

struct RecordingMarker {
    uint64_t time_ms = 0;
    RecordingMarkerType type = RecordingMarkerType::General;
    std::string label;

    bool operator==(const RecordingMarker& other) const noexcept {
        return time_ms == other.time_ms && type == other.type && label == other.label;
    }

    bool operator!=(const RecordingMarker& other) const noexcept {
        return !(*this == other);
    }
};

inline const char* RecordingMarkerTypeToString(RecordingMarkerType t) noexcept {
    switch (t) {
    case RecordingMarkerType::General:
        return "general";
    case RecordingMarkerType::Cut:
        return "cut";
    case RecordingMarkerType::Highlight:
        return "highlight";
    }
    return "general";
}

inline const char* RecordingMarkerTypeDefaultLabel(RecordingMarkerType t) noexcept {
    switch (t) {
    case RecordingMarkerType::General:
        return "Marker";
    case RecordingMarkerType::Cut:
        return "Cut";
    case RecordingMarkerType::Highlight:
        return "Highlight";
    }
    return "Marker";
}

// Maximum number of markers per recording session.
inline constexpr uint64_t kMaxRecordingMarkers = 10000;

// Partition session-timeline markers into one split segment and rebase them to
// segment-local time (SPLIT-RECORDING-R1). Selects markers in the half-open
// window [start_ms, start_ms + duration_ms): a marker exactly on the boundary
// (== the next segment start) is excluded so a paused-split marker is never
// duplicated into both segments — it lands in the next segment at 0 ms. Returns
// the rebased (time_ms -= start_ms) markers in input order.
[[nodiscard]] inline std::vector<RecordingMarker>
PartitionSegmentMarkers(const std::vector<RecordingMarker>& session_markers, uint64_t start_ms, uint64_t duration_ms) {
    std::vector<RecordingMarker> out;
    const uint64_t end_ms = start_ms + duration_ms;
    for (const auto& m : session_markers) {
        if (m.time_ms < start_ms || m.time_ms >= end_ms)
            continue;
        RecordingMarker rebased = m;
        rebased.time_ms = m.time_ms - start_ms;
        out.push_back(rebased);
    }
    return out;
}

} // namespace exosnap
