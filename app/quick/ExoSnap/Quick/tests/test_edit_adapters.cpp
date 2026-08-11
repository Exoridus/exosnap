#include "EditExportAdapter.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"
#include "EditTimelineModels.h"

#include "models/EditTimelineModel.h"

#include <QCoreApplication>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace exosnap::quick {
namespace {

// EditSessionAdapter posts its keyframe scan through the event loop and every
// adapter here is a QObject, so each case needs a live application object.
QCoreApplication* EnsureApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "edit_qml_adapter_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

RecordingMarker MakeMarker(uint64_t time_ms, const char* label = "m") {
    RecordingMarker marker;
    marker.time_ms = time_ms;
    marker.label = label;
    return marker;
}

// A fixture clip: no master path, so nothing is opened, decoded or remuxed.
EditContext MakeContext(double duration_seconds = 100.0) {
    EditContext context;
    context.output_path = QStringLiteral("D:/Recordings/clip.mkv");
    context.duration = QStringLiteral("1:40");
    context.size = QStringLiteral("120 MB");
    context.resolution = QStringLiteral("1920x1080");
    context.fps = QStringLiteral("60 fps CFR");
    context.video_codec = QStringLiteral("AV1 (NVENC)");
    context.audio_codec = QStringLiteral("Opus");
    context.container = QStringLiteral("MKV");
    context.duration_seconds = duration_seconds;
    return context;
}

// ── Trim snapping (pure) ────────────────────────────────────────────────────

TEST(EditTrimSnap, SnapsBackToTheKeyframeAtOrBeforeTheRequest) {
    const std::vector<int64_t> keyframes{0, 2'000'000, 4'000'000, 6'000'000};
    EXPECT_EQ(SnapTrimBoundaryUs(5'900'000, keyframes, {}), 4'000'000);
    EXPECT_EQ(SnapTrimBoundaryUs(4'000'000, keyframes, {}), 4'000'000);
    EXPECT_EQ(SnapTrimBoundaryUs(100, keyframes, {}), 0);
}

TEST(EditTrimSnap, WithoutAKeyframeTableTheRequestPassesThrough) {
    EXPECT_EQ(SnapTrimBoundaryUs(1'234'567, {}, {}), 1'234'567);
}

TEST(EditTrimSnap, AMarkerInsideTheWindowWinsOverTheKeyframe) {
    const std::vector<int64_t> keyframes{0, 2'000'000, 4'000'000};
    const std::vector<RecordingMarker> markers{MakeMarker(4030)};
    // Keyframe snap lands on 4.000 s; the marker at 4.030 s is 30 ms away, i.e.
    // inside the 50 ms window, so the cut moves to the marker.
    EXPECT_EQ(SnapTrimBoundaryUs(4'900'000, keyframes, markers), 4'030'000);
}

TEST(EditTrimSnap, AMarkerOutsideTheWindowLeavesTheKeyframeAlone) {
    const std::vector<int64_t> keyframes{0, 2'000'000, 4'000'000};
    const std::vector<RecordingMarker> markers{MakeMarker(4090)};
    EXPECT_EQ(SnapTrimBoundaryUs(4'900'000, keyframes, markers), 4'000'000);
}

// ── Trim lives once, in microseconds, snapped ───────────────────────────────

TEST(EditSessionAdapterTrim, StoresTheSnappedRangeOnceAndReportsItInMilliseconds) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    session.setKeyframeTimestampsForTest({0, 10'000'000, 20'000'000, 30'000'000, 40'000'000});

    session.requestTrim(24'000, 39'000);

    // Both boundaries snapped back to their keyframe; the millisecond accessors
    // are derived from the stored microseconds, never stored beside them.
    EXPECT_EQ(session.trimStartUs(), 20'000'000);
    EXPECT_EQ(session.trimEndUs(), 30'000'000);
    EXPECT_EQ(session.trimStartMs(), 20'000);
    EXPECT_EQ(session.trimEndMs(), 30'000);
    EXPECT_TRUE(session.trimmed());
}

TEST(EditSessionAdapterTrim, FullRangeIsNoTrimAtAll) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    session.setKeyframeTimestampsForTest({0, 10'000'000});

    session.requestTrim(0, 100'000);

    EXPECT_EQ(session.trimStartUs(), recorder_core::TrimRange::kNoTimestamp);
    EXPECT_EQ(session.trimEndUs(), recorder_core::TrimRange::kNoTimestamp);
    EXPECT_FALSE(session.trimmed());
    EXPECT_EQ(session.trimStartMs(), 0);
    EXPECT_EQ(session.trimEndMs(), 100'000);
}

TEST(EditSessionAdapterTrim, HandlesNeverCrossAndKeepTheMinimumGap) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());

    // Out-point dragged behind the in-point: clamping runs before the snap, so
    // the result is still a valid, ordered range.
    session.requestTrim(50'000, 10'000);

    EXPECT_LT(session.trimStartMs(), session.trimEndMs());
    EXPECT_GE(session.trimEndMs() - session.trimStartMs(), kMinTrimGapMs);
}

TEST(EditSessionAdapterTrim, ReleasingAHandleAsksForTheFrameAtThatBoundary) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    session.setKeyframeTimestampsForTest({0, 10'000'000, 20'000'000, 30'000'000});

    // Qt6::Test is not a component of this build, so the signal is captured with
    // a plain connection rather than a QSignalSpy.
    int seek_count = 0;
    qint64 seek_position = -1;
    QObject::connect(&session, &EditSessionAdapter::seekRequested, &session, [&](qint64 position_ms) {
        ++seek_count;
        seek_position = position_ms;
    });
    session.requestTrim(24'000, 100'000);

    ASSERT_EQ(seek_count, 1);
    EXPECT_EQ(seek_position, 20'000);
}

TEST(EditSessionAdapterTrim, ATrimCountsAsUnsavedWork) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    EXPECT_FALSE(session.hasUnsavedEdits());

    session.requestTrim(20'000, 40'000);
    EXPECT_TRUE(session.hasUnsavedEdits());
}

TEST(EditSessionAdapterReport, MapsPipelineHealthOntoTheHeaderSeverity) {
    EnsureApplication();
    EditSessionAdapter session;

    EditContext good = MakeContext();
    good.completed_snapshot.valid = true;
    good.completed_snapshot.health = recorder_core::PipelineHealth::Good;
    session.setEditContext(good);
    EXPECT_EQ(session.reportSeverityValue(), static_cast<int>(EditSessionAdapter::Neutral));
    EXPECT_TRUE(session.reportLabel().isEmpty());

    EditContext warning = MakeContext();
    warning.completed_snapshot.valid = true;
    warning.completed_snapshot.health = recorder_core::PipelineHealth::Warning;
    session.setEditContext(warning);
    EXPECT_EQ(session.reportSeverityValue(), static_cast<int>(EditSessionAdapter::Warning));
    EXPECT_EQ(session.reportLabel(), QStringLiteral("Warning"));

    EditContext critical = MakeContext();
    critical.completed_snapshot.valid = true;
    critical.completed_snapshot.health = recorder_core::PipelineHealth::Critical;
    session.setEditContext(critical);
    EXPECT_EQ(session.reportSeverityValue(), static_cast<int>(EditSessionAdapter::Critical));
}

TEST(EditSessionAdapterReport, CountsOnlyProblemDropsNotBenignCfrPacing) {
    EnsureApplication();
    EditSessionAdapter session;
    EditContext context = MakeContext();
    context.completed_snapshot.valid = true;
    context.completed_snapshot.capture.frames_emitted = 900;
    context.completed_snapshot.capture.frames_dropped_coalesced = 5000; // benign
    context.completed_snapshot.capture.frames_dropped_backpressure = 100;
    session.setEditContext(context);

    // 100 / (900 + 100) == 10.0%, i.e. the coalesced frames are not drops.
    EXPECT_TRUE(session.reportTooltip().contains(QStringLiteral("Frame drops: 10.0%")));
}

TEST(EditSessionAdapterFacts, RendersAnUnsetFactAsTheSharedEmptyGlyph) {
    EnsureApplication();
    EditSessionAdapter session;
    EditContext context = MakeContext();
    context.audio_codec.clear();
    session.setEditContext(context);

    ASSERT_EQ(session.facts().size(), 7);
    const QVariantMap audio_row = session.facts().at(5).toMap();
    EXPECT_EQ(audio_row.value(QStringLiteral("label")).toString(), QStringLiteral("Audio"));
    EXPECT_EQ(audio_row.value(QStringLiteral("value")).toString(), QString::fromUtf8("\xe2\x80\x94"));
}

// ── Export ─────────────────────────────────────────────────────────────────

TEST(EditExportPath, NewFileGetsTheEditSuffixAndTheSelectedExtension) {
    const std::filesystem::path original(L"D:/Recordings/clip.mkv");
    // Compared as paths, not strings: operator/ joins with the native separator.
    EXPECT_EQ(DeriveExportOutputPath(original, /*overwrite=*/false, /*to_mp4=*/false),
              std::filesystem::path(L"D:/Recordings/clip_edit.mkv"));
    EXPECT_EQ(DeriveExportOutputPath(original, /*overwrite=*/false, /*to_mp4=*/true),
              std::filesystem::path(L"D:/Recordings/clip_edit.mp4"));
}

TEST(EditExportPath, OverwriteWritesBackToTheOriginal) {
    const std::filesystem::path original(L"D:/Recordings/clip.mkv");
    EXPECT_EQ(DeriveExportOutputPath(original, /*overwrite=*/true, /*to_mp4=*/true), original);
}

TEST(EditExportProgress, OnlyAWholePercentChangeIsWorthCrossingTheThreadBoundary) {
    EXPECT_TRUE(ShouldPublishExportProgress(0.0f, -1));
    EXPECT_FALSE(ShouldPublishExportProgress(0.421f, 42));
    EXPECT_FALSE(ShouldPublishExportProgress(0.429f, 42));
    EXPECT_TRUE(ShouldPublishExportProgress(0.43f, 42));
    EXPECT_TRUE(ShouldPublishExportProgress(1.0f, 99));
}

TEST(EditExportAdapterState, RunningIsOwnedHereAndMirroredOntoTheSession) {
    EnsureApplication();
    EditSessionAdapter session;
    EditExportAdapter exporter;
    exporter.setSession(&session);
    session.setEditContext(MakeContext());

    EXPECT_FALSE(exporter.running());
    EXPECT_FALSE(session.exportRunning());
    EXPECT_TRUE(exporter.canExport());

    exporter.applyVisualState(EditExportAdapter::Running, 10, QString(), QString());
    EXPECT_TRUE(exporter.running());
    EXPECT_TRUE(session.exportRunning());
    EXPECT_FALSE(exporter.canExport());

    exporter.applyVisualState(EditExportAdapter::Done, 100, QStringLiteral("D:/out.mkv"), QString());
    EXPECT_FALSE(exporter.running());
    EXPECT_FALSE(session.exportRunning());
    EXPECT_TRUE(exporter.canExport());
}

TEST(EditExportAdapterState, CancelKeepsTheRunRunningUntilTheThreadReportsBack) {
    EnsureApplication();
    EditSessionAdapter session;
    EditExportAdapter exporter;
    exporter.setSession(&session);
    session.setEditContext(MakeContext());

    exporter.applyVisualState(EditExportAdapter::Running, 30, QString(), QString());
    exporter.cancel();

    // The Widgets surface cleared its own running flag here while the remux
    // thread was still winding down, which is what let a Retry land inside the
    // deferred join.
    EXPECT_EQ(exporter.stateValue(), static_cast<int>(EditExportAdapter::Cancelling));
    EXPECT_TRUE(exporter.running());
    EXPECT_FALSE(exporter.canExport());
}

TEST(EditExportAdapterOptions, DestinationLineFollowsTheSelectedSaveMode) {
    EnsureApplication();
    EditExportAdapter exporter;

    exporter.setSaveModeKey(QStringLiteral("new"));
    exporter.setContainerKey(QStringLiteral("mp4"));
    EXPECT_TRUE(exporter.destinationText().contains(QStringLiteral("_edit.mp4")));
    EXPECT_FALSE(exporter.overwriteSelected());

    exporter.setSaveModeKey(QStringLiteral("overwrite"));
    EXPECT_TRUE(exporter.overwriteSelected());
    EXPECT_TRUE(exporter.destinationText().contains(QStringLiteral("Replaces the original")));
}

TEST(EditExportAdapterOptions, AnUnknownKeyFallsBackToTheShippedDefault) {
    EnsureApplication();
    EditExportAdapter exporter;
    exporter.setContainerKey(QStringLiteral("webm"));
    exporter.setSaveModeKey(QStringLiteral("append"));
    EXPECT_EQ(exporter.containerKey(), QStringLiteral("mkv"));
    EXPECT_EQ(exporter.saveModeKey(), QStringLiteral("new"));
}

TEST(EditExportAdapterRun, AClipWithoutAnEditMasterFailsBeforeAnyThreadIsStarted) {
    EnsureApplication();
    EditSessionAdapter session;
    EditExportAdapter exporter;
    exporter.setSession(&session);
    session.setEditContext(MakeContext()); // fixture context: no mkv_master_path

    exporter.startExport();

    EXPECT_EQ(exporter.stateValue(), static_cast<int>(EditExportAdapter::Failed));
    EXPECT_FALSE(exporter.running());
    EXPECT_EQ(exporter.errorText(), QStringLiteral("No edit master available for export."));
}

// ── Timeline models ────────────────────────────────────────────────────────

TEST(TimelineMarkerThinning, DropsMarkersThatCollapseOntoTheSamePixelColumn) {
    std::vector<RecordingMarker> markers;
    for (uint64_t ms = 0; ms < 400; ++ms)
        markers.push_back(MakeMarker(ms));

    // 400 markers across 100 s on a 200 px track: they all land in the first
    // pixel column, so exactly one survives.
    const auto visible = VisibleTimelineMarkers(markers, 100'000, 200);
    EXPECT_EQ(visible.size(), 1U);
}

TEST(TimelineMarkerThinning, NeverExceedsTheRenderCapEvenOnAWideTrack) {
    std::vector<RecordingMarker> markers;
    for (uint64_t i = 0; i < 10'000; ++i)
        markers.push_back(MakeMarker(i * 10));

    const auto visible = VisibleTimelineMarkers(markers, 100'000, 100'000);
    EXPECT_LE(static_cast<int>(visible.size()), kMaxRenderedMarkers);
}

TEST(TimelineMarkerThinning, AnInertTimelineShowsNoMarkers) {
    const std::vector<RecordingMarker> markers{MakeMarker(1000)};
    EXPECT_TRUE(VisibleTimelineMarkers(markers, 0, 800).empty());
    EXPECT_TRUE(VisibleTimelineMarkers(markers, 100'000, 0).empty());
}

TEST(TimelineTileModel, DelegatesGetAProviderUrlKeyedByRunAndIndexNeverAnImage) {
    EditTimelineTileModel model;
    model.beginRun(7);
    model.appendTile(1500);
    model.appendTile(3000);

    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.data(model.index(1, 0), EditTimelineTileModel::SourceRole).toString(),
              QStringLiteral("image://exoedittile/7/1"));
    EXPECT_EQ(model.data(model.index(0, 0), EditTimelineTileModel::TimeMsRole).toLongLong(), 1500);
    EXPECT_EQ(model.roleNames().value(EditTimelineTileModel::SourceRole), QByteArrayLiteral("tileSource"));
}

TEST(TimelineTileModel, ANewRunInvalidatesEveryPreviouslyPublishedTile) {
    EditTimelineTileModel model;
    model.beginRun(1);
    model.appendTile(0);
    model.beginRun(2);
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.runId(), 2U);
}

TEST(TimelineTileProvider, RefusesTilesFromARunTheStripHasMovedPast) {
    EditTimelineTileProvider provider;
    provider.submitTile(3, 0, QImage(4, 4, QImage::Format_ARGB32));
    QSize size;
    EXPECT_FALSE(provider.requestImage(QStringLiteral("3/0"), &size, {}).isNull());
    // A newer run replaces the strip wholesale rather than accumulating.
    provider.submitTile(4, 0, QImage(4, 4, QImage::Format_ARGB32));
    EXPECT_TRUE(provider.requestImage(QStringLiteral("3/0"), &size, {}).isNull());
    EXPECT_FALSE(provider.requestImage(QStringLiteral("4/0"), &size, {}).isNull());
}

TEST(TimelineRowBudget, ThreeUnmergedAudioRowsStillFitTheSharedStackBudget) {
    EXPECT_EQ(TimelineAudioRowHeight(0), 0);
    EXPECT_EQ(TimelineAudioStackHeight(0), 0);
    EXPECT_EQ(TimelineAudioRowHeight(1), kTimelineAudioRowHeight);
    EXPECT_LE(TimelineAudioStackHeight(3), kTimelineAudioStackBudget + 3 * kTimelineRowGap);
    // A row too small to name is worse than one row fewer.
    EXPECT_GE(TimelineAudioRowHeight(3), kTimelineAudioRowMinHeight);
}

} // namespace
} // namespace exosnap::quick
