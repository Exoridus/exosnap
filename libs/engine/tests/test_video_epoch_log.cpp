// The video-epoch record is read by an analyzer outside the recorder, so its
// field names, its unit and its source marker are a contract, not a log message.

#include <gtest/gtest.h>

#include "qpc_100ns.h"
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

// The conversion that produces every 100 ns value in this contract, including the
// overlay acknowledgement's. Pinned here because a wrong result is not a wrong
// shape: it is a well-formed number on the wrong timeline.

TEST(QpcTicks, ConvertsWholeAndFractionalSeconds) {
    EXPECT_EQ(exosnap::engine::QpcTicksTo100ns(0, 10'000'000), 0u);
    EXPECT_EQ(exosnap::engine::QpcTicksTo100ns(10'000'000, 10'000'000), 10'000'000u);
    // A 10 MHz counter is already in 100 ns units, so the value passes through.
    EXPECT_EQ(exosnap::engine::QpcTicksTo100ns(4'089'402'061'399, 10'000'000), 4'089'402'061'399u);
    // 3.6864 MHz, the other frequency Windows commonly reports.
    EXPECT_EQ(exosnap::engine::QpcTicksTo100ns(3'686'400, 3'686'400), 10'000'000u);
    EXPECT_EQ(exosnap::engine::QpcTicksTo100ns(1'843'200, 3'686'400), 5'000'000u);
}

TEST(QpcTicks, SurvivesTheUptimeAtWhichTheDirectFormOverflows) {
    // `ticks * 10'000'000` leaves the 64-bit range at about 9.2e11 ticks -- a day
    // of uptime on a 10 MHz counter. The direct form does not fail there, it
    // wraps: the reading below came back as 40'012 s instead of 408'940 s, which
    // is what made an overlay acknowledgement land 368'928 s before the recording
    // it belonged to.
    constexpr uint64_t kFourDaysOfTicks = 4'089'402'061'399;
    constexpr uint64_t kTenMhz = 10'000'000;
    EXPECT_EQ(exosnap::engine::QpcTicksTo100ns(kFourDaysOfTicks, kTenMhz) / 10'000'000ULL, 408'940u);

    // The same instant on a 24 MHz counter, well past the same boundary.
    EXPECT_EQ(exosnap::engine::QpcTicksTo100ns(408'940ULL * 24'000'000ULL, 24'000'000) / 10'000'000ULL, 408'940u);
}

TEST(QpcTicks, AZeroFrequencyYieldsZeroRatherThanDividing) {
    EXPECT_EQ(exosnap::engine::QpcTicksTo100ns(500, 0), 0u);
}
