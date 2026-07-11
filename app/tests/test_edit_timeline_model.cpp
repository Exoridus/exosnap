#include <gtest/gtest.h>

#include <QDir>
#include <QString>
#include <QTemporaryDir>

#include <filesystem>
#include <vector>

#include "models/EditTimelineModel.h"
#include "models/MarkerSidecar.h"
#include "models/RecordingMarker.h"

namespace exosnap {
namespace {

RecordingMarker MakeMarker(uint64_t time_ms, const char* label = "Marker") {
    RecordingMarker m;
    m.time_ms = time_ms;
    m.type = RecordingMarkerType::General;
    m.label = label;
    return m;
}

// ---- Timestamp formatting (drag feedback label) ----

TEST(EditTimelineModel, TimestampOmitsHoursForShortRecordings) {
    // Total < 1 h: "MM:SS.mmm", all fields two-digit.
    EXPECT_EQ(FormatTimelineTimestamp(0, 258000), QStringLiteral("00:00.000"));
    EXPECT_EQ(FormatTimelineTimestamp(31482, 258000), QStringLiteral("00:31.482"));
    EXPECT_EQ(FormatTimelineTimestamp(258000, 258000), QStringLiteral("04:18.000"));
}

TEST(EditTimelineModel, TimestampShowsHoursForLongRecordings) {
    // Total >= 1 h: "HH:MM:SS.mmm".
    const qint64 total = 2 * 3600000; // 2 h
    EXPECT_EQ(FormatTimelineTimestamp(0, total), QStringLiteral("00:00:00.000"));
    EXPECT_EQ(FormatTimelineTimestamp(3723456, total), QStringLiteral("01:02:03.456"));
}

TEST(EditTimelineModel, TimestampClampsNegativePositions) {
    EXPECT_EQ(FormatTimelineTimestamp(-500, 10000), QStringLiteral("00:00.000"));
}

TEST(EditTimelineModel, ClockFormatFollowsTheSameHourRule) {
    EXPECT_EQ(FormatTimelineClock(258000, 258000), QStringLiteral("04:18"));
    EXPECT_EQ(FormatTimelineClock(3723000, 2 * 3600000), QStringLiteral("01:02:03"));
}

// ---- Trim handle clamping ----

TEST(EditTimelineModel, TrimHandlesConstrainEachOther) {
    // Start can never reach (or cross) the end handle.
    EXPECT_EQ(ClampTrimStartMs(90000, 60000), 60000 - kMinTrimGapMs);
    // End can never reach (or cross) the start handle.
    EXPECT_EQ(ClampTrimEndMs(10000, 30000, 100000), 30000 + kMinTrimGapMs);
    // In-range values pass through untouched.
    EXPECT_EQ(ClampTrimStartMs(15000, 60000), 15000);
    EXPECT_EQ(ClampTrimEndMs(90000, 30000, 100000), 90000);
    // And both respect the clip bounds.
    EXPECT_EQ(ClampTrimStartMs(-5, 60000), 0);
    EXPECT_EQ(ClampTrimEndMs(200000, 30000, 100000), 100000);
}

TEST(EditTimelineModel, PlayheadClampsToClip) {
    EXPECT_EQ(ClampPlayheadMs(-10, 100000), 0);
    EXPECT_EQ(ClampPlayheadMs(50000, 100000), 50000);
    EXPECT_EQ(ClampPlayheadMs(999999, 100000), 100000);
}

// ---- Marker retiming for trimmed exports ----

TEST(EditTimelineModel, RetimeShiftsSurvivorsAndDropsTrimmedMarkers) {
    const std::vector<RecordingMarker> markers = {
        MakeMarker(5000, "before-in"),   // dropped (before trim start)
        MakeMarker(30000, "at-in"),      // kept, rebased to 0
        MakeMarker(95000, "inside"),     // kept, rebased to 65000
        MakeMarker(200000, "at-out"),    // dropped (half-open window)
        MakeMarker(250000, "after-out"), // dropped
    };
    const auto out = RetimeMarkersForTrim(markers, 30000, 200000);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].time_ms, 0u);
    EXPECT_EQ(out[0].label, "at-in");
    EXPECT_EQ(out[1].time_ms, 65000u);
    EXPECT_EQ(out[1].label, "inside");
}

TEST(EditTimelineModel, RetimeWithEmptyOrInvertedWindowYieldsNothing) {
    const std::vector<RecordingMarker> markers = {MakeMarker(1000)};
    EXPECT_TRUE(RetimeMarkersForTrim(markers, 50000, 50000).empty());
    EXPECT_TRUE(RetimeMarkersForTrim(markers, 60000, 50000).empty());
}

// ---- Marker sidecar export plan ----

TEST(MarkerSidecarExport, SidecarPathReplacesTheMediaExtension) {
    EXPECT_EQ(DeriveMarkerSidecarPath("C:/videos/clip.mkv"), std::filesystem::path("C:/videos/clip.markers.json"));
    EXPECT_EQ(DeriveMarkerSidecarPath("C:/videos/clip_edit.mp4"),
              std::filesystem::path("C:/videos/clip_edit.markers.json"));
    EXPECT_TRUE(DeriveMarkerSidecarPath({}).empty());
}

TEST(MarkerSidecarExport, PlanWritesOnlyWhenMarkersSurvive) {
    const auto with_markers = PlanMarkerSidecarForExport("C:/videos/clip_edit.mkv", {MakeMarker(1000)});
    EXPECT_TRUE(with_markers.shouldWrite());
    EXPECT_EQ(with_markers.media, QStringLiteral("clip_edit.mkv"));

    const auto without_markers = PlanMarkerSidecarForExport("C:/videos/clip_edit.mkv", {});
    EXPECT_FALSE(without_markers.shouldWrite());
    EXPECT_EQ(without_markers.sidecar_path, std::filesystem::path("C:/videos/clip_edit.markers.json"));
}

TEST(MarkerSidecarExport, ApplyWritesARetimedSidecarNextToTheExport) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path media(QDir(tmp.path()).filePath(QStringLiteral("clip_edit.mkv")).toStdWString());

    const auto plan = PlanMarkerSidecarForExport(media, {MakeMarker(0, "at-in"), MakeMarker(65000, "inside")});
    ASSERT_TRUE(ApplyMarkerExportPlan(plan));

    const auto roundtrip = ReadMarkerSidecar(plan.sidecar_path);
    ASSERT_EQ(roundtrip.size(), 2u);
    EXPECT_EQ(roundtrip[0].time_ms, 0u);
    EXPECT_EQ(roundtrip[1].time_ms, 65000u);
    EXPECT_EQ(roundtrip[1].label, "inside");
}

TEST(MarkerSidecarExport, ApplyRemovesAStaleSidecarWhenNoMarkersSurvive) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path media(QDir(tmp.path()).filePath(QStringLiteral("clip.mkv")).toStdWString());

    // Simulate an overwrite-original export: a sidecar from the recording
    // session already sits next to the media file.
    ASSERT_TRUE(ApplyMarkerExportPlan(PlanMarkerSidecarForExport(media, {MakeMarker(1000)})));
    const auto sidecar = DeriveMarkerSidecarPath(media);
    ASSERT_TRUE(std::filesystem::exists(sidecar));

    // The trim cut every marker away: the stale sidecar must disappear.
    ASSERT_TRUE(ApplyMarkerExportPlan(PlanMarkerSidecarForExport(media, {})));
    EXPECT_FALSE(std::filesystem::exists(sidecar));
}

} // namespace
} // namespace exosnap
