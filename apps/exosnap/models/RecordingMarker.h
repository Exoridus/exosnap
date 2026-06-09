#pragma once

#include <cstdint>
#include <string>

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

} // namespace exosnap
