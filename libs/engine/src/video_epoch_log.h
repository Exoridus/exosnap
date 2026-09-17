#pragma once

// The video epoch as a structured log record: the instant video PTS 0 sits on,
// expressed on the QPC axis, so a measurement taken outside the recorder can
// place its own events on the recording's timeline.
//
// Without it the only way to relate the two timelines is to find the first frame
// in which the thing being measured appears and call that its first event, which
// derives the offset from the thing under test and therefore measures nothing.

#include <exosnap/engine/logging/logging.h>

#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::engine {

// Which reading opened the video timeline.
enum class VideoEpochSource {
    // The QPC reading taken when the capture path observed the first frame. It
    // trails the frame's actual present by up to one acquire interval.
    CaptureObserved,
    // The first frame's own present timestamp, which sits exactly on the QPC axis.
    FrameTimestamp,
    // The session start, used because the first frame carried a present timestamp
    // from before recording began. It is a floor, not the instant of any frame, so
    // an offset measured against it is a bound and not a reading.
    SessionStartFloor,
};

inline constexpr const char* kVideoEpochLogMessage = "video epoch established";

// Which source a VFR epoch came from. The clamp that keeps the timeline from
// starting before the recording did replaces the frame's timestamp with the
// session start, and the two are worth different things to a measurement -- so
// the record says which one was published rather than which one was offered.
[[nodiscard]] inline VideoEpochSource VfrVideoEpochSource(std::int64_t published_ticks_100ns,
                                                          std::int64_t frame_ticks_100ns) noexcept {
    return published_ticks_100ns == frame_ticks_100ns ? VideoEpochSource::FrameTimestamp
                                                      : VideoEpochSource::SessionStartFloor;
}

[[nodiscard]] inline const char* VideoEpochSourceName(VideoEpochSource source) noexcept {
    switch (source) {
    case VideoEpochSource::FrameTimestamp:
        return "frame_timestamp";
    case VideoEpochSource::SessionStartFloor:
        return "session_start_floor";
    case VideoEpochSource::CaptureObserved:
        break;
    }
    return "capture_observed";
}

// Fields for the video-epoch record. `epoch_qpc_100ns` is the published epoch in
// the same 100 ns QPC-derived units the engine's timestamps use; a reader
// converts its own counter readings with `qpc_frequency_hz`, which is stated so a
// reader that assumed a fixed tick rate is contradicted rather than believed.
[[nodiscard]] inline std::vector<logging::LogField>
VideoEpochLogFields(std::uint64_t epoch_qpc_100ns, std::uint64_t qpc_frequency_hz, VideoEpochSource source) {
    return {
        {"video_epoch_qpc_100ns", std::to_string(epoch_qpc_100ns)},
        {"qpc_frequency_hz", std::to_string(qpc_frequency_hz)},
        {"epoch_source", VideoEpochSourceName(source)},
    };
}

} // namespace exosnap::engine
