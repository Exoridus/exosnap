#include "EditExportAdapter.h"
#include "EditRenderText.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"
#include "EditTimelineModels.h"

#include "models/EditTimelineModel.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
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

TEST(EditRenderPresentation, LocalizesPolicyAndPreservesTechnicalDetail) {
    EnsureApplication();
    class Translator final : public QTranslator {
      public:
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* source, const char*, int) const override {
            return QByteArray(context) == "EditRender" && QByteArray(source) == "The timeline is empty."
                       ? QStringLiteral("Die Zeitleiste ist leer.")
                       : QString();
        }
    } translator;
    ASSERT_TRUE(QCoreApplication::installTranslator(&translator));
    EXPECT_EQ(TranslateEditRenderReason(QStringLiteral("The timeline is empty. (code=7)")),
              QStringLiteral("Die Zeitleiste ist leer. (code=7)"));
    QCoreApplication::removeTranslator(&translator);
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

TEST(EditWorkspacePresentation, SelectingEitherLinkedRowHighlightsTheWholeGroup) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    auto rows = session.visibleClips(0, 100000);
    ASSERT_EQ(rows.size(), 2);
    const auto video = rows[0].toMap();
    const auto audio = rows[1].toMap();
    EXPECT_TRUE(video.value(QStringLiteral("video")).toBool());
    EXPECT_FALSE(audio.value(QStringLiteral("video")).toBool());
    EXPECT_EQ(video.value(QStringLiteral("group")), audio.value(QStringLiteral("group")));
    EXPECT_EQ(video.value(QStringLiteral("path")), audio.value(QStringLiteral("path")));
    session.selectClip(audio.value(QStringLiteral("id")).toULongLong());
    rows = session.visibleClips(0, 100000);
    EXPECT_TRUE(rows[0].toMap().value(QStringLiteral("selected")).toBool());
    EXPECT_TRUE(rows[1].toMap().value(QStringLiteral("selected")).toBool());
}

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

    EXPECT_EQ(session.trimStartUs(), exosnap::engine::TrimRange::kNoTimestamp);
    EXPECT_EQ(session.trimEndUs(), exosnap::engine::TrimRange::kNoTimestamp);
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

TEST(EditSessionAdapterTrim, DraggingTheOutPointSeeksToTheOutPoint) {
    // The defect. The seek target was chosen by testing the IN-point's value
    // (`clamped_start <= 0 ? end_us : start_us`), not by which handle moved. With
    // an in-point already set, dragging the out-point sent the preview back to
    // the start of the range -- away from the frame the user was cutting at.
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    session.setKeyframeTimestampsForTest({0, 10'000'000, 20'000'000, 30'000'000, 40'000'000, 50'000'000});

    // An in-point is set first, and its seek is consumed.
    session.requestTrim(20'000, 100'000);
    ASSERT_EQ(session.trimStartUs(), 20'000'000);

    int seek_count = 0;
    qint64 seek_position = -1;
    QObject::connect(&session, &EditSessionAdapter::seekRequested, &session, [&](qint64 position_ms) {
        ++seek_count;
        seek_position = position_ms;
    });

    // Now only the out-point moves. The in-point is unchanged at 20 s.
    session.requestTrim(20'000, 44'000);

    ASSERT_EQ(session.trimStartUs(), 20'000'000) << "the in-point must not have moved";
    ASSERT_EQ(session.trimEndUs(), 40'000'000);
    ASSERT_EQ(seek_count, 1);
    EXPECT_EQ(seek_position, 40'000) << "the seek must follow the handle that moved, not the in-point";
}

TEST(EditSessionAdapterTrim, DraggingTheInPointStillSeeksToTheInPoint) {
    // The control: the case the old rule got right must stay right.
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    session.setKeyframeTimestampsForTest({0, 10'000'000, 20'000'000, 30'000'000, 40'000'000});

    session.requestTrim(20'000, 40'000);

    int seek_count = 0;
    qint64 seek_position = -1;
    QObject::connect(&session, &EditSessionAdapter::seekRequested, &session, [&](qint64 position_ms) {
        ++seek_count;
        seek_position = position_ms;
    });

    session.requestTrim(31'000, 40'000);

    ASSERT_EQ(seek_count, 1);
    EXPECT_EQ(seek_position, 30'000);
}

TEST(EditSessionAdapterTrim, ATrimThatChangesNothingAsksForNoSeek) {
    // Re-applying the same range is not a drag. A seek here would fight the
    // playhead the user just moved somewhere else.
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    session.setKeyframeTimestampsForTest({0, 10'000'000, 20'000'000, 30'000'000});

    session.requestTrim(20'000, 30'000);

    int seek_count = 0;
    QObject::connect(&session, &EditSessionAdapter::seekRequested, &session, [&](qint64) { ++seek_count; });
    session.requestTrim(20'000, 30'000);

    EXPECT_EQ(seek_count, 0);
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
    good.completed_snapshot.health = exosnap::engine::PipelineHealth::Good;
    session.setEditContext(good);
    EXPECT_EQ(session.reportSeverityValue(), static_cast<int>(EditSessionAdapter::Neutral));
    // The header states a verdict in every case, so a healthy recording names
    // itself rather than falling back to the word "Report".
    EXPECT_EQ(session.reportLabel(), QStringLiteral("Good"));

    EditContext warning = MakeContext();
    warning.completed_snapshot.valid = true;
    warning.completed_snapshot.health = exosnap::engine::PipelineHealth::Warning;
    session.setEditContext(warning);
    EXPECT_EQ(session.reportSeverityValue(), static_cast<int>(EditSessionAdapter::Warning));
    EXPECT_EQ(session.reportLabel(), QStringLiteral("Warning"));

    EditContext critical = MakeContext();
    critical.completed_snapshot.valid = true;
    critical.completed_snapshot.health = exosnap::engine::PipelineHealth::Critical;
    session.setEditContext(critical);
    EXPECT_EQ(session.reportSeverityValue(), static_cast<int>(EditSessionAdapter::Critical));
    EXPECT_EQ(session.reportLabel(), QStringLiteral("Critical"));
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

TEST(EditWorkspaceAdapter, EqualLengthAppendNotifiesCumulativeDurationAndRetainsExistingClips) {
    EnsureApplication();
    EditSessionAdapter session;
    std::vector<qint64> durations;
    int page_requests = 0;
    QObject::connect(&session, &EditSessionAdapter::durationChanged, &session,
                     [&]() { durations.push_back(session.durationMs()); });
    QObject::connect(&session, &EditSessionAdapter::editPageRequested, &session, [&]() { ++page_requests; });

    session.setEditContext(MakeContext(10));
    const auto original = session.workspace().clips();
    ASSERT_EQ(original.size(), 2U);
    session.setEditContext(MakeContext(10));

    EXPECT_EQ(durations, (std::vector<qint64>{10000, 20000}));
    EXPECT_EQ(page_requests, 2);
    EXPECT_EQ(session.positionMs(), 10000);
    ASSERT_EQ(session.workspace().clips().size(), 4U);
    for (const auto& clip : original) {
        const auto* retained = session.workspace().clip(clip.id);
        ASSERT_NE(retained, nullptr);
        EXPECT_EQ(*retained, clip);
    }
    EXPECT_EQ(session.workspace().assets().size(), 1U);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), original);
    EXPECT_EQ(session.durationMs(), 10000);
}

TEST(EditWorkspaceAdapter, PositionedSegmentsAreConsecutiveAndUndoAsOneOperation) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto original = session.workspace().clips();
    EditContext split = MakeContext(5);
    split.segments = {{QStringLiteral("D:/Recordings/segment-0.mkv"), 2, true},
                      {QStringLiteral("D:/Recordings/segment-1.mkv"), 3, true}};
    session.setEditContext(split, 20000);

    EXPECT_TRUE(session.workspaceError().isEmpty());
    EXPECT_EQ(session.durationMs(), 25000);
    EXPECT_EQ(session.positionMs(), 20000);
    const auto positioned = session.workspace().clips();
    ASSERT_EQ(positioned.size(), 6U);
    for (const auto type : {edit::TrackType::Video, edit::TrackType::Audio}) {
        const auto* first = session.workspace().active(20000000, type);
        const auto* second = session.workspace().active(22000000, type);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(first->start, 20000000);
        EXPECT_EQ(first->duration(), 2000000);
        EXPECT_EQ(second->start, 22000000);
        EXPECT_EQ(second->duration(), 3000000);
        EXPECT_EQ(session.workspace().asset(first->asset)->name, "segment-0.mkv");
        EXPECT_EQ(session.workspace().asset(second->asset)->name, "segment-1.mkv");
    }
    session.undo();
    EXPECT_EQ(session.workspace().clips(), original);
    EXPECT_EQ(session.durationMs(), 10000);
    session.redo();
    EXPECT_EQ(session.workspace().clips(), positioned);
    EXPECT_EQ(session.durationMs(), 25000);
}

TEST(EditWorkspaceAdapter, OverlappingSegmentBatchChangesNoClipsAndAddsNoUndoCommand) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto original = session.workspace().clips();
    EditContext split = MakeContext(5);
    split.segments = {{QStringLiteral("D:/Recordings/segment-0.mkv"), 2, true},
                      {QStringLiteral("D:/Recordings/segment-1.mkv"), 3, true}};

    session.setEditContext(split, 9000);

    EXPECT_FALSE(session.workspaceError().isEmpty());
    EXPECT_EQ(session.workspace().clips(), original);
    EXPECT_EQ(session.durationMs(), 10000);
    session.undo();
    EXPECT_TRUE(session.workspace().clips().empty());
    EXPECT_FALSE(session.canUndo());
}

TEST(EditWorkspaceAdapter, SplitMarkersUseHalfOpenSourceWindowsAndRetainTheirLabels) {
    EnsureApplication();
    EditSessionAdapter session;
    EditContext split = MakeContext(5);
    split.segments = {{QStringLiteral("D:/Recordings/segment-0.mkv"), 2, true},
                      {QStringLiteral("D:/Recordings/segment-1.mkv"), 3, true}};
    split.markers = {MakeMarker(0, "start"), MakeMarker(1999, "before"), MakeMarker(2000, "cut"),
                     MakeMarker(4999, "last"), MakeMarker(5000, "outside")};

    session.setEditContext(split, 10000);

    const auto& assets = session.workspace().assets();
    ASSERT_EQ(assets.size(), 2U);
    ASSERT_EQ(assets[0].markers.size(), 2U);
    ASSERT_EQ(assets[1].markers.size(), 2U);
    EXPECT_EQ(assets[0].markers[0].time_ms, 0U);
    EXPECT_EQ(assets[0].markers[1].time_ms, 1999U);
    EXPECT_EQ(assets[0].markers[1].label, "before");
    EXPECT_EQ(assets[1].markers[0].time_ms, 0U);
    EXPECT_EQ(assets[1].markers[0].label, "cut");
    EXPECT_EQ(assets[1].markers[1].time_ms, 2999U);
    EXPECT_EQ(assets[1].markers[1].label, "last");
    EXPECT_EQ(session.workspace().snap(12030000, 0, 50000), 12000000);
}

TEST(EditWorkspaceAdapter, MissingAndFailedSegmentsRemainVisibleAtTheirOriginalDurations) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    EditSessionAdapter session;
    EditContext split = MakeContext(5);
    split.segments = {{directory.filePath(QStringLiteral("missing.mkv")), 2, true},
                      {directory.filePath(QStringLiteral("failed.mkv")), 3, false}};

    session.setEditContext(split);

    ASSERT_EQ(session.workspace().assets().size(), 2U);
    EXPECT_EQ(session.workspace().assets()[0].state, edit::AssetState::Missing);
    EXPECT_EQ(session.workspace().assets()[1].state, edit::AssetState::Failed);
    EXPECT_EQ(session.durationMs(), 5000);
    const auto first = session.visibleClips(100, 1900);
    const auto second = session.visibleClips(2100, 4900);
    ASSERT_EQ(first.size(), 2);
    ASSERT_EQ(second.size(), 2);
    for (const auto& row : first) {
        EXPECT_FALSE(row.toMap().value(QStringLiteral("available")).toBool());
        EXPECT_EQ(row.toMap().value(QStringLiteral("durationMs")).toLongLong(), 2000);
    }
    for (const auto& row : second) {
        EXPECT_FALSE(row.toMap().value(QStringLiteral("available")).toBool());
        EXPECT_EQ(row.toMap().value(QStringLiteral("startMs")).toLongLong(), 2000);
        EXPECT_EQ(row.toMap().value(QStringLiteral("durationMs")).toLongLong(), 3000);
    }
}

TEST(EditWorkspaceAdapter, ViewportFollowsLinkedTrimMoveAndUndoWithoutDuplicatingMedia) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto original = session.workspace().clips();
    const auto selected = session.selectedClip();

    session.trimClip(selected, 2000, 8000);
    EXPECT_TRUE(session.visibleClips(0, 1000).isEmpty());
    ASSERT_EQ(session.visibleClips(3000, 4000).size(), 2);
    const auto trimmed = session.workspace().clips();
    for (const auto& clip : trimmed) {
        EXPECT_EQ(clip.start, 2000000);
        EXPECT_EQ(clip.source_in, 2000000);
        EXPECT_EQ(clip.source_out, 8000000);
    }
    session.moveClip(selected, 20000, false);
    EXPECT_TRUE(session.visibleClips(3000, 4000).isEmpty());
    EXPECT_EQ(session.visibleClips(21000, 22000).size(), 2);
    EXPECT_EQ(session.durationMs(), 26000);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), trimmed);
    EXPECT_EQ(session.visibleClips(3000, 4000).size(), 2);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), original);
    session.redo();
    EXPECT_EQ(session.workspace().clips(), trimmed);
    EXPECT_EQ(session.media().size(), 1);
}

TEST(EditWorkspaceAdapter, SplitAndRippleDeleteRestoreBothLinkedTracksWithUndo) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto original = session.workspace().clips();
    session.requestSeek(4000);
    session.splitSelected();
    const auto split = session.workspace().clips();
    ASSERT_EQ(split.size(), 4U);
    session.deleteSelected(true);

    ASSERT_EQ(session.workspace().clips().size(), 2U);
    EXPECT_EQ(session.durationMs(), 6000);
    for (const auto& clip : session.workspace().clips()) {
        EXPECT_EQ(clip.start, 0);
        EXPECT_EQ(clip.source_in, 4000000);
        EXPECT_EQ(clip.source_out, 10000000);
    }
    session.undo();
    EXPECT_EQ(session.workspace().clips(), split);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), original);
}

TEST(EditWorkspaceAdapter, LocalMediaBatchPreservesInputOrderAndUndoesAsOneOperation) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString first = directory.filePath(QStringLiteral("b.mp4"));
    const QString second = directory.filePath(QStringLiteral("a.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, first));
    ASSERT_TRUE(QFile::copy(fixture, second));
    EditSessionAdapter session;
    QEventLoop loop;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    session.importMediaBatch({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)}, true, 2000);
    loop.exec();

    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    ASSERT_EQ(session.workspace().assets().size(), 2U);
    const auto& assets = session.workspace().assets();
    EXPECT_EQ(assets[0].path, first.toStdWString());
    EXPECT_EQ(assets[1].path, second.toStdWString());
    ASSERT_GT(assets[0].duration, 0);
    EXPECT_FALSE(assets[0].audio);
    ASSERT_EQ(session.workspace().clips().size(), 2U);
    EXPECT_EQ(session.workspace().clips()[0].start, 2000000);
    EXPECT_EQ(session.workspace().clips()[1].start, 2000000 + assets[0].duration);
    const auto imported = session.workspace().clips();
    session.undo();
    EXPECT_TRUE(session.workspace().clips().empty());
    EXPECT_EQ(session.media().size(), 2);
    session.redo();
    EXPECT_EQ(session.workspace().clips(), imported);
}

TEST(EditWorkspaceAdapter, UnknownSegmentDurationIsResolvedBeforeOrderedPositionedInsertion) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString unknown = directory.filePath(QStringLiteral("first.mp4"));
    const QString known = directory.filePath(QStringLiteral("second.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, unknown));
    ASSERT_TRUE(QFile::copy(fixture, known));
    EditSessionAdapter session;
    session.setEditContext(MakeContext(1));
    const auto original = session.workspace().clips();
    EditContext split = MakeContext(0);
    split.segments = {{unknown, 0, true}, {known, 2, true}};
    QEventLoop loop;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &loop, [&]() {
        if (session.workspace().clips().size() == 6)
            loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);

    session.setEditContext(split, 3000);
    loop.exec();

    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    ASSERT_EQ(session.workspace().assets().size(), 3U);
    ASSERT_EQ(session.workspace().clips().size(), 6U);
    const auto& resolved = session.workspace().assets()[1];
    ASSERT_GT(resolved.duration, 0);
    EXPECT_EQ(resolved.path, unknown.toStdWString());
    const auto* first = session.workspace().active(3000000);
    const auto* second = session.workspace().active(3000000 + resolved.duration);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->asset, resolved.id);
    EXPECT_EQ(first->start, 3000000);
    EXPECT_EQ(session.workspace().asset(second->asset)->path, known.toStdWString());
    EXPECT_EQ(second->start, 3000000 + resolved.duration);
    EXPECT_EQ(second->duration(), 2000000);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), original);
}

// ── Export ─────────────────────────────────────────────────────────────────

TEST(EditExportPath, DefaultDestinationUsesConfiguredFolderAndSelectedContainer) {
    const std::filesystem::path directory(L"D:/Configured output");
    EXPECT_EQ(DefaultEditExportPath(directory, false), directory / L"ExoSnap-export.mkv");
    EXPECT_EQ(DefaultEditExportPath(directory, true), directory / L"ExoSnap-export.mp4");
}

TEST(EditExportPath, DefaultDestinationDoesNotAddAnEditSubdirectory) {
    const std::filesystem::path directory(L"D:/Configured output");
    const auto destination = DefaultEditExportPath(directory, false);
    EXPECT_EQ(destination.parent_path(), directory);
    EXPECT_EQ(destination.filename(), L"ExoSnap-export.mkv");
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
    EXPECT_TRUE(exporter.destinationText().contains(QStringLiteral("Choose a filename and folder")));
    EXPECT_TRUE(exporter.destinationText().contains(QStringLiteral("mp4")));
    EXPECT_FALSE(exporter.destinationText().contains(QStringLiteral("_edit")));
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

TEST(EditExportAdapterOptions, MatchSourceIsTheDefaultAndRenderProfilesCannotBeSelected) {
    EnsureApplication();
    EditExportAdapter exporter;
    EXPECT_EQ(exporter.profileKey(), QStringLiteral("match"));
    const auto profiles = exporter.profileOptions();
    ASSERT_EQ(profiles.size(), 5);
    for (const auto& row : profiles) {
        const auto profile = row.toMap();
        const auto key = profile.value(QStringLiteral("value")).toString();
        if (key == QStringLiteral("match")) {
            EXPECT_TRUE(profile.value(QStringLiteral("selectable")).toBool());
        } else {
            EXPECT_FALSE(profile.value(QStringLiteral("selectable")).toBool());
            EXPECT_FALSE(profile.value(QStringLiteral("reason")).toString().isEmpty());
            exporter.setProfileKey(key);
            EXPECT_EQ(exporter.profileKey(), QStringLiteral("match"));
        }
    }
}

TEST(EditExportAdapterRun, AnEmptyWorkspaceFailsBeforeAnyThreadIsStarted) {
    EnsureApplication();
    EditSessionAdapter session;
    EditExportAdapter exporter;
    exporter.setSession(&session);

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

// ── The decoding path the fixtures never reach ──────────────────────────────

// Every other case above uses MakeContext(), which deliberately has no master
// path — so the timeline never creates its thumbnail source and never wires it
// up. That blind spot hid a real defect: the source's signals were connected on
// every trimSnapReadyChanged with Qt::UniqueConnection and a lambda receiver, a
// combination Qt does not support. A debug build asserted and abort()ed the
// process on the first genuine clip; a release build silently accumulated a
// duplicate connection pair per clip, double-counting decoded tiles.
//
// This case sets a master path so the wiring actually runs. The file need not
// exist: the connection is made before the decoder is asked to open anything,
// and a failed open is reported through the same signals rather than by
// crashing. What is being pinned is that opening a second clip does not
// re-connect anything.
TEST(EditTimelineAdapterSource, WiringTheDecoderSourceSurvivesASecondClip) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);

    EditContext context = MakeContext();
    context.mkv_master_path = QStringLiteral("D:/Recordings/does-not-exist.mkv");
    session.setEditContext(context);
    session.setKeyframeTimestampsForTest({0, 2'000'000, 4'000'000});

    // Second clip, same adapter — the path that used to add a duplicate pair of
    // connections and count each tile of this clip twice.
    EditContext second = MakeContext(50.0);
    second.mkv_master_path = QStringLiteral("D:/Recordings/also-missing.mkv");
    session.setEditContext(second);
    session.setKeyframeTimestampsForTest({0, 1'000'000});

    EXPECT_TRUE(session.trimSnapReady());
    // Nothing decoded — neither file exists — so the strip stays empty rather
    // than reporting phantom progress.
    EXPECT_EQ(timeline.tilesReady(), 0);
}

// ── Closing the session closes the clip (QCR-301) ──────────────────────────

// Records the order of the two teardown signals: the resources must be released
// before the surface is asked to go away, not after it already did.
struct CloseTrace {
    int clip_closed = 0;
    int close_requested = 0;
    QStringList order;
};

void TraceClose(EditSessionAdapter& session, CloseTrace& trace) {
    QObject::connect(&session, &EditSessionAdapter::clipClosed, &session, [&trace]() {
        ++trace.clip_closed;
        trace.order.append(QStringLiteral("clipClosed"));
    });
    QObject::connect(&session, &EditSessionAdapter::closeRequested, &session, [&trace]() {
        ++trace.close_requested;
        trace.order.append(QStringLiteral("closeRequested"));
    });
}

TEST(EditSessionAdapterClose, ReleasesTheClipBeforeDismissingTheSurface) {
    EnsureApplication();
    EditSessionAdapter session;
    EditContext context = MakeContext();
    context.mkv_master_path = QStringLiteral("D:/Recordings/clip.mkv");
    session.setEditContext(context);
    session.requestTrim(20'000, 40'000);
    ASSERT_TRUE(session.open());

    CloseTrace trace;
    TraceClose(session, trace);
    session.close();

    EXPECT_EQ(trace.clip_closed, 1);
    EXPECT_EQ(trace.close_requested, 1);
    EXPECT_EQ(trace.order, (QStringList{QStringLiteral("clipClosed"), QStringLiteral("closeRequested")}));
    EXPECT_FALSE(session.open());
    EXPECT_EQ(session.durationMs(), 0);
    EXPECT_TRUE(session.clipPath().isEmpty());
    EXPECT_TRUE(session.markers().empty());
    EXPECT_TRUE(session.keyframeTimestamps().empty());
    EXPECT_FALSE(session.trimSnapReady());
    EXPECT_FALSE(session.hasUnsavedEdits());
}

TEST(EditSessionAdapterClose, AFixtureClipWithoutAMasterClosesJustAsCompletely) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext()); // duration, no master path
    ASSERT_FALSE(session.open());
    ASSERT_GT(session.durationMs(), 0);

    CloseTrace trace;
    TraceClose(session, trace);
    session.close();

    EXPECT_EQ(trace.clip_closed, 1);
    EXPECT_EQ(session.durationMs(), 0);
}

TEST(EditSessionAdapterClose, IsIdempotentAndKeepsDismissingTheSurface) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());

    CloseTrace trace;
    TraceClose(session, trace);
    session.close();
    session.close();

    // The second close has nothing left to release, but the request to dismiss
    // the surface is not swallowed — a stuck overlay is worse than a repeat.
    EXPECT_EQ(trace.clip_closed, 1);
    EXPECT_EQ(trace.close_requested, 2);
}

TEST(EditSessionAdapterClose, ClosingAnEmptySessionOnlyDismissesTheSurface) {
    EnsureApplication();
    EditSessionAdapter session;

    CloseTrace trace;
    TraceClose(session, trace);
    session.close();

    EXPECT_EQ(trace.clip_closed, 0);
    EXPECT_EQ(trace.close_requested, 1);
}

TEST(EditSessionAdapterClose, ANewSessionAfterACloseOpensNormally) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);

    EditContext first = MakeContext();
    first.mkv_master_path = QStringLiteral("D:/Recordings/first.mkv");
    session.setEditContext(first);
    session.close();

    EditContext second = MakeContext(50.0);
    second.mkv_master_path = QStringLiteral("D:/Recordings/second.mkv");
    second.markers = {MakeMarker(1000)};
    session.setEditContext(second);

    EXPECT_TRUE(session.open());
    EXPECT_EQ(session.durationMs(), 50'000);
    EXPECT_EQ(session.markers().size(), 1U);
    EXPECT_EQ(timeline.tilesReady(), 0);
}

TEST(EditTimelineAdapterClose, ClosingTheSessionEmptiesTheStripAndTheRun) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeContext());
    timeline.setTrackWidth(800);
    timeline.setFixture({QStringLiteral("System")}, 3);
    ASSERT_EQ(timeline.tilesReady(), 3);
    ASSERT_NE(timeline.activeRunForTest(), 0U);

    session.close();

    EXPECT_EQ(timeline.tilesReady(), 0);
    EXPECT_EQ(timeline.tilesExpected(), 0);
    EXPECT_EQ(timeline.activeRunForTest(), 0U);
    EXPECT_TRUE(timeline.audioTrackLabels().isEmpty());
}

// ── A previous clip's tiles never reach the next one (QCR-302) ─────────────

TEST(TimelineTileRunGate, WaitsUntilTheDecoderSourceIsOnTheClipTheStripShows) {
    const QString clip_a = QStringLiteral("D:/Recordings/a.mkv");
    const QString clip_b = QStringLiteral("D:/Recordings/b.mkv");
    // The window between "the session announced clip B" and "the source finished
    // reopening on B" — a run started here would decode B's timestamps out of A.
    EXPECT_FALSE(TimelineTileRunAllowed(true, clip_b, clip_a, 12));
    EXPECT_TRUE(TimelineTileRunAllowed(true, clip_b, clip_b, 12));
    EXPECT_FALSE(TimelineTileRunAllowed(false, clip_b, clip_b, 12));
    EXPECT_FALSE(TimelineTileRunAllowed(true, QString(), QString(), 12));
    EXPECT_FALSE(TimelineTileRunAllowed(true, clip_b, clip_b, 0));
}

TEST(EditTimelineAdapterGeneration, ATileFromThePreviousClipCannotReachTheNewStrip) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeContext());
    timeline.setTrackWidth(800);
    timeline.setFixture({QStringLiteral("System")}, 2);

    const quint64 first_run = timeline.activeRunForTest();
    ASSERT_NE(first_run, 0U);
    const int before = timeline.tilesReady();
    // A tile of the run in flight still counts.
    timeline.deliverTileForTest(1000, QImage(4, 4, QImage::Format_ARGB32), first_run);
    ASSERT_EQ(timeline.tilesReady(), before + 1);

    // Second clip. The decode for the first one is still on its way here.
    EditContext second = MakeContext(50.0);
    second.mkv_master_path = QStringLiteral("D:/Recordings/second.mkv");
    session.setEditContext(second);
    ASSERT_EQ(timeline.activeRunForTest(), 0U);
    ASSERT_EQ(timeline.tilesReady(), 0);

    timeline.deliverTileForTest(1000, QImage(4, 4, QImage::Format_ARGB32), first_run);
    timeline.deliverTileForTest(2000, QImage(4, 4, QImage::Format_ARGB32), first_run);

    // Nothing from the previous clip's generation moved the new clip's state.
    EXPECT_EQ(timeline.tilesReady(), 0);
    EXPECT_EQ(timeline.activeRunForTest(), 0U);
}

TEST(EditTimelineAdapterGeneration, ARunIdOfZeroIsNeverAccepted) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeContext());

    timeline.deliverTileForTest(1000, QImage(4, 4, QImage::Format_ARGB32), 0);
    EXPECT_EQ(timeline.tilesReady(), 0);
}

// ── The strip has a terminal state (QCR-307) ───────────────────────────────
//
// tilesExpected > 0 with tilesReady == 0 described two situations that are not
// the same thing: a clip that is mid-decode, and a clip that carries nothing
// decodable and never will. The strip showed "Generating previews…" for both,
// so the second one showed it for the rest of the session.
//
// The two endings are driven through the same signals the worker emits. A real
// one needs media: a working clip for the first, a deliberately broken one for
// the second, and neither is something a fixture carries.

// A clip that will never produce a tile is fixture-free by construction — the
// fixture path bypasses the decoder entirely — so these drive a real (missing)
// master path and then deliver the worker's verdict.
EditContext MakeUndecodableContext() {
    EditContext context = MakeContext();
    context.mkv_master_path = QStringLiteral("D:/Recordings/not-a-real-clip.mkv");
    return context;
}

TEST(EditTimelinePreviewState, AClipWithNothingToShowIsIdleNotGenerating) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);

    EXPECT_EQ(timeline.previewState(), QStringLiteral("idle"));
    EXPECT_FALSE(timeline.generatingPreviews());
    EXPECT_FALSE(timeline.previewsUnavailable());
}

TEST(EditTimelinePreviewState, APartlyFilledStripKeepsSayingItIsGenerating) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeContext());
    timeline.setTrackWidth(800);
    // Fewer tiles than the row holds: exactly the state a slow decode is in.
    timeline.setFixture({QStringLiteral("System")}, 2);

    ASSERT_GT(timeline.tilesExpected(), timeline.tilesReady());
    EXPECT_EQ(timeline.previewState(), QStringLiteral("generating"));
    EXPECT_TRUE(timeline.generatingPreviews());
    EXPECT_FALSE(timeline.previewsUnavailable());
}

TEST(EditTimelinePreviewState, AFullStripIsReady) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeContext());
    timeline.setTrackWidth(800);
    timeline.setFixture({QStringLiteral("System")}, -1);

    ASSERT_EQ(timeline.tilesReady(), timeline.tilesExpected());
    ASSERT_GT(timeline.tilesExpected(), 0);
    EXPECT_EQ(timeline.previewState(), QStringLiteral("ready"));
    EXPECT_FALSE(timeline.generatingPreviews());
}

TEST(EditTimelinePreviewState, AClipTheEngineCannotOpenIsUnavailableAtOnce) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeUndecodableContext());
    timeline.setTrackWidth(800);

    // 0x0 is the worker's answer for "could not open, or carries no video". The
    // strip does not have to wait for a run to prove what the open already said.
    timeline.deliverClipOpenedForTest(0, 0, {});

    EXPECT_EQ(timeline.previewState(), QStringLiteral("unavailable"));
    EXPECT_TRUE(timeline.previewsUnavailable());
    EXPECT_FALSE(timeline.generatingPreviews()) << "the honest state replaces the optimistic one, it does not join it";
}

TEST(EditTimelinePreviewState, ARunThatEndsWithNoTileAtAllIsUnavailable) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeContext());
    timeline.setTrackWidth(800);
    timeline.setFixture({QStringLiteral("System")}, 0);
    const quint64 run = timeline.activeRunForTest();
    ASSERT_NE(run, 0U);
    ASSERT_EQ(timeline.previewState(), QStringLiteral("generating"));

    // The clip opened, and the decoder still produced nothing for any position.
    timeline.deliverRunFinishedForTest(run, 0, /*cancelled=*/false);

    EXPECT_EQ(timeline.previewState(), QStringLiteral("unavailable"));
}

TEST(EditTimelinePreviewState, ACancelledRunIsNotAFailure) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeContext());
    timeline.setTrackWidth(800);
    timeline.setFixture({QStringLiteral("System")}, 0);
    const quint64 run = timeline.activeRunForTest();
    ASSERT_NE(run, 0U);

    // A resize, a clip switch or a close abandons the run in flight. It says
    // nothing about the clip, and must not put "Preview unavailable" on a
    // recording the user merely resized the window for.
    timeline.deliverRunFinishedForTest(run, 0, /*cancelled=*/true);

    EXPECT_FALSE(timeline.previewsUnavailable());
    EXPECT_EQ(timeline.previewState(), QStringLiteral("generating"));
}

TEST(EditTimelinePreviewState, ARunTheStripHasMovedPastCannotFailIt) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeContext());
    timeline.setTrackWidth(800);
    timeline.setFixture({QStringLiteral("System")}, 0);
    const quint64 stale_run = timeline.activeRunForTest() + 99;

    timeline.deliverRunFinishedForTest(stale_run, 0, /*cancelled=*/false);
    timeline.deliverRunFinishedForTest(0, 0, /*cancelled=*/false);

    EXPECT_FALSE(timeline.previewsUnavailable());
}

TEST(EditTimelinePreviewState, ARunThatProducedTilesIsNeverUnavailable) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeContext());
    timeline.setTrackWidth(800);
    timeline.setFixture({QStringLiteral("System")}, -1);
    const quint64 run = timeline.activeRunForTest();
    ASSERT_NE(run, 0U);
    ASSERT_GT(timeline.tilesReady(), 0);

    timeline.deliverRunFinishedForTest(run, timeline.tilesReady(), /*cancelled=*/false);

    EXPECT_FALSE(timeline.previewsUnavailable());
    EXPECT_EQ(timeline.previewState(), QStringLiteral("ready"));
}

TEST(EditTimelinePreviewState, AFailedClipDoesNotFollowTheUserToTheNextOne) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeUndecodableContext());
    timeline.setTrackWidth(800);
    timeline.deliverClipOpenedForTest(0, 0, {});
    ASSERT_TRUE(timeline.previewsUnavailable());

    // Clip B is a different question, and it starts unanswered.
    EditContext second = MakeContext(50.0);
    second.mkv_master_path = QStringLiteral("D:/Recordings/second.mkv");
    session.setEditContext(second);

    EXPECT_FALSE(timeline.previewsUnavailable());
    EXPECT_NE(timeline.previewState(), QStringLiteral("unavailable"));
}

TEST(EditTimelinePreviewState, ClosingTheSessionClearsTheVerdictWithTheStrip) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeUndecodableContext());
    timeline.setTrackWidth(800);
    timeline.deliverClipOpenedForTest(0, 0, {});
    ASSERT_TRUE(timeline.previewsUnavailable());

    session.close();

    EXPECT_FALSE(timeline.previewsUnavailable());
    EXPECT_EQ(timeline.previewState(), QStringLiteral("idle"));
}

// The rest of the Edit surface does not depend on thumbnails, and an
// unavailable strip must not take any of it away.
TEST(EditTimelinePreviewState, AnUnavailableStripLeavesTrimAndMarkersWorking) {
    EnsureApplication();
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    session.setEditContext(MakeUndecodableContext());
    timeline.setTrackWidth(800);
    timeline.deliverClipOpenedForTest(0, 0, {});
    ASSERT_TRUE(timeline.previewsUnavailable());

    session.requestTrim(10'000, 30'000);
    EXPECT_EQ(session.trimStartMs(), 10'000);
    EXPECT_EQ(session.trimEndMs(), 30'000);
    EXPECT_GT(session.durationMs(), 0);
    EXPECT_NE(timeline.markerModel(), nullptr);
}

} // namespace
} // namespace exosnap::quick
