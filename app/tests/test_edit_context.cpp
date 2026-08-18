// test_edit_context.cpp — tests for the canonical marker-sidecar serialization.
//
// Exercises the SHARED functions in models/MarkerSidecar.h that both
// RecordingCoordinator (write on AddMarker / on stop / per segment) and
// EditExportPage (load + write-back in the edit surface) use. There is exactly
// one format and one writer; these tests validate that single implementation.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QJsonValue>
#include <QTemporaryDir>

#include <filesystem>
#include <limits>

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

// ---------------------------------------------------------------------------
// QCR-207: `timeMs` validation at the JSON → typed-marker boundary.
//
// The parse was `static_cast<uint64_t>(value.toDouble())`. toDouble() answers
// 0.0 for a missing key, a string, a bool and null, so an unusable marker
// silently became a marker at 00:00 — close enough to a trim boundary for
// SnapTrimBoundaryUs to snap the trim to the clip start. And casting a negative
// or out-of-range double to uint64_t is undefined behaviour: -1 came out as
// 2^64-1, which the same consumer reads back as int64_t -1 and multiplies to a
// negative microsecond position.
//
// The valid contract is derived from the two sides, not invented: a JSON number
// in [0, 2^53]. Above 2^53 a double is no longer an exact integer millisecond
// count, and 2^53 ms in microseconds still fits int64_t.
// ---------------------------------------------------------------------------

namespace {

std::vector<exosnap::RecordingMarker> ParseMarkers(const char* json, int* skipped = nullptr) {
    return exosnap::ParseMarkerSidecar(QByteArray(json), skipped);
}

} // namespace

TEST(MarkerSidecarTimeTest, ZeroIsAValidPosition) {
    // 0 ms is the clip start, a legitimate marker position — not a sentinel.
    const auto markers = ParseMarkers(R"({"markers":[{"timeMs":0,"type":"cut","label":"start"}]})");
    ASSERT_EQ(markers.size(), 1u);
    EXPECT_EQ(markers[0].time_ms, 0u);
    EXPECT_EQ(markers[0].type, exosnap::RecordingMarkerType::Cut);
}

TEST(MarkerSidecarTimeTest, PositiveIntegerIsAccepted) {
    const auto markers = ParseMarkers(R"({"markers":[{"timeMs":12345}]})");
    ASSERT_EQ(markers.size(), 1u);
    EXPECT_EQ(markers[0].time_ms, 12345u);
}

TEST(MarkerSidecarTimeTest, MissingTimeIsSkippedInsteadOfBecomingZero) {
    int skipped = 0;
    const auto markers = ParseMarkers(R"({"markers":[{"type":"cut","label":"no time"}]})", &skipped);
    EXPECT_TRUE(markers.empty()) << "a marker with no time is not a marker at 00:00";
    EXPECT_EQ(skipped, 1);
}

TEST(MarkerSidecarTimeTest, WrongTypesAreSkipped) {
    for (const char* json : {
             R"({"markers":[{"timeMs":"1234"}]})",
             R"({"markers":[{"timeMs":true}]})",
             R"({"markers":[{"timeMs":null}]})",
             R"({"markers":[{"timeMs":[1234]}]})",
             R"({"markers":[{"timeMs":{"ms":1234}}]})",
         }) {
        int skipped = 0;
        EXPECT_TRUE(ParseMarkers(json, &skipped).empty()) << json;
        EXPECT_EQ(skipped, 1) << json;
    }
}

TEST(MarkerSidecarTimeTest, NegativeTimeIsSkipped) {
    // The undefined-behaviour case: static_cast<uint64_t>(-1.0).
    int skipped = 0;
    EXPECT_TRUE(ParseMarkers(R"({"markers":[{"timeMs":-1}]})", &skipped).empty());
    EXPECT_EQ(skipped, 1);
    EXPECT_TRUE(ParseMarkers(R"({"markers":[{"timeMs":-0.5}]})").empty());
}

TEST(MarkerSidecarTimeTest, TheLargestRepresentableTimeIsStillAccepted) {
    // 2^53 exactly — the boundary is inclusive, and the pre-existing
    // LargeTimestamp round-trip at 2^53-1 must keep working.
    const auto markers = ParseMarkers(R"({"markers":[{"timeMs":9007199254740992}]})");
    ASSERT_EQ(markers.size(), 1u);
    EXPECT_EQ(markers[0].time_ms, 9007199254740992ULL);
}

TEST(MarkerSidecarTimeTest, BeyondTheRepresentableRangeIsSkipped) {
    // Just past 2^53, and far past it. Both used to be an out-of-range
    // double→uint64_t cast, and the second would additionally overflow the
    // consumer's `* 1000` into int64_t microseconds.
    int skipped = 0;
    EXPECT_TRUE(ParseMarkers(R"({"markers":[{"timeMs":9007199254740994}]})", &skipped).empty());
    EXPECT_EQ(skipped, 1);
    EXPECT_TRUE(ParseMarkers(R"({"markers":[{"timeMs":1e30}]})").empty());
    EXPECT_TRUE(ParseMarkers(R"({"markers":[{"timeMs":1.8e19}]})").empty());
}

TEST(MarkerSidecarTimeTest, NonFiniteValuesAreRejectedAtTheHelper) {
    // Not reachable through a parsed file — NaN and infinity have no JSON
    // syntax, and Qt's parser refuses them — so this is asserted against the
    // boundary helper directly rather than by inventing an impossible document.
    EXPECT_FALSE(exosnap::ParseMarkerTimeMs(QJsonValue(std::numeric_limits<double>::quiet_NaN())).has_value());
    EXPECT_FALSE(exosnap::ParseMarkerTimeMs(QJsonValue(std::numeric_limits<double>::infinity())).has_value());
    EXPECT_FALSE(exosnap::ParseMarkerTimeMs(QJsonValue(-std::numeric_limits<double>::infinity())).has_value());
    // Sanity: the same helper does accept an ordinary value.
    EXPECT_EQ(exosnap::ParseMarkerTimeMs(QJsonValue(42.0)).value_or(999u), 42u);
}

TEST(MarkerSidecarTimeTest, ValidMarkersSurviveAlongsideInvalidOnes) {
    // The established contract for a damaged sidecar is lenient, never
    // all-or-nothing: ReadMarkerSidecar already returns what it could read
    // rather than failing the open. Per-marker validation follows the same rule.
    int skipped = 0;
    const auto markers = ParseMarkers(R"({"markers":[
        {"timeMs":1000,"type":"general","label":"good one"},
        {"timeMs":-5,"type":"cut","label":"negative"},
        {"type":"cut","label":"no time"},
        {"timeMs":"2000","label":"string"},
        {"timeMs":3000,"type":"highlight","label":"good two"}
    ]})",
                                      &skipped);

    ASSERT_EQ(markers.size(), 2u);
    EXPECT_EQ(markers[0].time_ms, 1000u);
    EXPECT_EQ(markers[0].label, "good one");
    EXPECT_EQ(markers[1].time_ms, 3000u);
    EXPECT_EQ(markers[1].type, exosnap::RecordingMarkerType::Highlight);
    EXPECT_EQ(skipped, 3);
}

TEST(MarkerSidecarTimeTest, TypeAndLabelStayLenient) {
    // Only `timeMs` became strict. An unknown type still falls back to General
    // and a missing label to empty — that leniency is the existing contract and
    // is not what QCR-207 was about.
    int skipped = 0;
    const auto markers = ParseMarkers(R"({"markers":[{"timeMs":700,"type":"nonsense"}]})", &skipped);
    ASSERT_EQ(markers.size(), 1u);
    EXPECT_EQ(markers[0].type, exosnap::RecordingMarkerType::General);
    EXPECT_TRUE(markers[0].label.empty());
    EXPECT_EQ(skipped, 0);
}

TEST(MarkerSidecarTimeTest, ReadMarkerSidecarReportsSkippedMarkers) {
    QTemporaryDir dir;
    const auto path = SidecarPath(dir);
    {
        QFile f(QString::fromStdWString(path.wstring()));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(R"({"version":1,"markers":[{"timeMs":10},{"timeMs":-1},{"timeMs":20}]})");
    }

    int skipped = -1;
    const auto markers = exosnap::ReadMarkerSidecar(path, &skipped);
    ASSERT_EQ(markers.size(), 2u);
    EXPECT_EQ(skipped, 1);

    // The counter is reset even on the paths that never reach the parser, so a
    // caller cannot read a stale count from a previous file.
    int untouched = 7;
    EXPECT_TRUE(exosnap::ReadMarkerSidecar(std::filesystem::path{}, &untouched).empty());
    EXPECT_EQ(untouched, 0);
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
