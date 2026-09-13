// The video-epoch record is read by an analyzer outside the recorder, so its
// field names, its unit and its source marker are a contract, not a log message.

#include <gtest/gtest.h>

#include "video_epoch_log.h"

#include <optional>
#include <string>

namespace {

std::optional<std::string> FieldValue(const std::vector<exosnap::engine::logging::LogField>& fields,
                                      std::string_view key) {
    for (const auto& field : fields) {
        if (field.key == key)
            return field.value;
    }
    return std::nullopt;
}

} // namespace

TEST(VideoEpochLog, CarriesTheEpochItsUnitAndTheCounterFrequency) {
    const auto fields = exosnap::engine::VideoEpochLogFields(123456789012345ULL, 10000000ULL,
                                                             exosnap::engine::VideoEpochSource::CaptureObserved);

    EXPECT_EQ(FieldValue(fields, "video_epoch_qpc_100ns"), "123456789012345");
    EXPECT_EQ(FieldValue(fields, "qpc_frequency_hz"), "10000000");
}

TEST(VideoEpochLog, SaysWhichReadingOpenedTheTimeline) {
    // The three readings do not carry the same uncertainty: a frame's own present
    // timestamp is exact on the QPC axis, the capture-observed reading trails it by
    // up to one acquire interval, and the session-start floor is not the instant of
    // any frame at all. A reader that cannot tell them apart has to assume the worst
    // of the three for all of them.
    const auto observed =
        exosnap::engine::VideoEpochLogFields(1, 10000000ULL, exosnap::engine::VideoEpochSource::CaptureObserved);
    const auto stamped =
        exosnap::engine::VideoEpochLogFields(1, 10000000ULL, exosnap::engine::VideoEpochSource::FrameTimestamp);
    const auto floored =
        exosnap::engine::VideoEpochLogFields(1, 10000000ULL, exosnap::engine::VideoEpochSource::SessionStartFloor);

    EXPECT_EQ(FieldValue(observed, "epoch_source"), "capture_observed");
    EXPECT_EQ(FieldValue(stamped, "epoch_source"), "frame_timestamp");
    EXPECT_EQ(FieldValue(floored, "epoch_source"), "session_start_floor");
}

TEST(VideoEpochLog, AZeroFrequencyIsReportedRatherThanHiddenBehindADefault) {
    // QueryPerformanceFrequency cannot fail on any supported Windows version, but
    // the field is what a reader validates its own conversion against. Silently
    // substituting a plausible rate would make a wrong conversion look checked.
    const auto fields = exosnap::engine::VideoEpochLogFields(500, 0, exosnap::engine::VideoEpochSource::FrameTimestamp);

    EXPECT_EQ(FieldValue(fields, "qpc_frequency_hz"), "0");
}

TEST(VideoEpochLog, AClampedVfrEpochIsNotReportedAsTheFramesOwnTimestamp) {
    // The clamp exists because a first frame can carry a present timestamp from
    // before recording began. What it publishes is then the session start, and a
    // measurement told "frame_timestamp" would treat that floor as an instant.
    EXPECT_EQ(exosnap::engine::VfrVideoEpochSource(500, 500), exosnap::engine::VideoEpochSource::FrameTimestamp);
    EXPECT_EQ(exosnap::engine::VfrVideoEpochSource(500, 400), exosnap::engine::VideoEpochSource::SessionStartFloor);
}
