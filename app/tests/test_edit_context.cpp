// test_edit_context.cpp — tests for the canonical marker-sidecar serialization.
//
// Exercises the SHARED functions in models/MarkerSidecar.h that both
// RecordingCoordinator (write on AddMarker / on stop / per segment) and
// EditExportPage (load + write-back in the edit surface) use. There is exactly
// one format and one writer; these tests validate that single implementation.

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <filesystem>

#include "../models/MarkerSidecar.h"
#include "../models/RecordingMarker.h"

namespace {

std::filesystem::path SidecarPath(const QTemporaryDir& dir) {
    return std::filesystem::path(dir.path().toStdWString()) / L"test.markers.json";
}

} // namespace

TEST(MarkerSidecarTest, RoundTrip) {
    QTemporaryDir dir;
    const auto path = SidecarPath(dir);
    std::vector<exosnap::RecordingMarker> markers = {
        {1000, exosnap::RecordingMarkerType::General, "Start"},
        {5000, exosnap::RecordingMarkerType::Cut, "Cut here"},
        {9999, exosnap::RecordingMarkerType::Highlight, "Clip"},
    };
    ASSERT_TRUE(exosnap::WriteMarkerSidecar(path, markers, QStringLiteral("clip.mp4")));
    const auto loaded = exosnap::ReadMarkerSidecar(path);
    ASSERT_EQ(loaded.size(), markers.size());
    for (size_t i = 0; i < markers.size(); ++i) {
        EXPECT_EQ(loaded[i].time_ms, markers[i].time_ms);
        EXPECT_EQ(loaded[i].type, markers[i].type);
        EXPECT_EQ(loaded[i].label, markers[i].label);
    }
}

TEST(MarkerSidecarTest, EmptyMarkersWriteReadEmpty) {
    QTemporaryDir dir;
    const auto path = SidecarPath(dir);
    ASSERT_TRUE(exosnap::WriteMarkerSidecar(path, {}));
    EXPECT_TRUE(exosnap::ReadMarkerSidecar(path).empty());
}

TEST(MarkerSidecarTest, MissingFileReturnsEmpty) {
    QTemporaryDir dir;
    // Never wrote — read must return empty, not crash.
    EXPECT_TRUE(exosnap::ReadMarkerSidecar(SidecarPath(dir)).empty());
}

TEST(MarkerSidecarTest, EmptyPathIsNoop) {
    // Empty path must not write and must read as empty.
    EXPECT_FALSE(exosnap::WriteMarkerSidecar(std::filesystem::path{}, {}));
    EXPECT_TRUE(exosnap::ReadMarkerSidecar(std::filesystem::path{}).empty());
}

TEST(MarkerSidecarTest, TypeStringsRoundTrip) {
    using T = exosnap::RecordingMarkerType;
    QTemporaryDir dir;
    const auto path = SidecarPath(dir);
    std::vector<exosnap::RecordingMarker> markers = {
        {0, T::General, "g"},
        {100, T::Cut, "c"},
        {200, T::Highlight, "h"},
    };
    ASSERT_TRUE(exosnap::WriteMarkerSidecar(path, markers));
    const auto loaded = exosnap::ReadMarkerSidecar(path);
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded[0].type, T::General);
    EXPECT_EQ(loaded[1].type, T::Cut);
    EXPECT_EQ(loaded[2].type, T::Highlight);
}

TEST(MarkerSidecarTest, LargeTimestamp) {
    QTemporaryDir dir;
    const auto path = SidecarPath(dir);
    // Ensure uint64 timestamps survive the qint64/double JSON round-trip.
    // Largest safe integer for JSON double: 2^53 - 1 = 9007199254740991 ms
    // (~285 000 years), well above any realistic recording duration.
    const uint64_t big = 9007199254740991ULL;
    std::vector<exosnap::RecordingMarker> markers = {{big, exosnap::RecordingMarkerType::General, "end"}};
    ASSERT_TRUE(exosnap::WriteMarkerSidecar(path, markers));
    const auto loaded = exosnap::ReadMarkerSidecar(path);
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].time_ms, big);
}

TEST(MarkerSidecarTest, SerializeOmitsEmptyMediaIncludesSegmentIndex) {
    // media empty => no "media" key; segment_index set => "segmentIndex" present.
    const auto doc = exosnap::SerializeMarkerSidecar({{0, exosnap::RecordingMarkerType::General, "m"}}, QString{},
                                                     /*segment_index=*/2);
    const auto root = doc.object();
    EXPECT_FALSE(root.contains(QStringLiteral("media")));
    ASSERT_TRUE(root.contains(QStringLiteral("segmentIndex")));
    EXPECT_EQ(root.value(QStringLiteral("segmentIndex")).toInt(), 2);
    EXPECT_EQ(root.value(QStringLiteral("version")).toInt(), 1);
    EXPECT_EQ(root.value(QStringLiteral("timebase")).toString(), QStringLiteral("milliseconds"));
}

TEST(MarkerSidecarTest, CoordinatorFormatHasMediaNoSegmentIndex) {
    // The single-file coordinator path passes a media name and no segment index.
    const auto doc = exosnap::SerializeMarkerSidecar({{0, exosnap::RecordingMarkerType::General, "m"}},
                                                     QStringLiteral("rec.mkv"), std::nullopt);
    const auto root = doc.object();
    EXPECT_EQ(root.value(QStringLiteral("media")).toString(), QStringLiteral("rec.mkv"));
    EXPECT_FALSE(root.contains(QStringLiteral("segmentIndex")));
}
