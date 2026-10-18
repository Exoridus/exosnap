#include "EditExportAdapter.h"
#include "EditRenderText.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"
#include "EditTimelineModels.h"
#include "EditViewportAdapter.h"

#include "models/EditTimelineModel.h"
#include "models/OutputSettingsModel.h"
#include "models/RecordingCapabilities.h"
#include "models/VideoSettingsModel.h"

#include <capability/capability_builder.h>
#include <capability/encoder_device_resolver.h>
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline_export.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTranslator>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace exosnap::quick {
class EditExportAdapterTestPeer {
  public:
    static QThread* worker(const EditExportAdapter& adapter) {
        return adapter.export_thread_;
    }
    static int progress(const EditExportAdapter& adapter) {
        return adapter.run_->percent.load();
    }
    static void observeSourceVerification(EditExportAdapter& adapter, std::function<void(uint64_t)> observer) {
        adapter.source_verified_ = std::move(observer);
    }
};

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

// ── Edit outcomes ───────────────────────────────────────────────────────────

TEST(EditWorkspaceOutcome, EveryEditAttemptIsCountedWithItsAnswer) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    const auto video = session.visibleClips(0, 100000)[0].toMap().value(QStringLiteral("id")).toULongLong();
    const auto start = session.editOutcomeSequence();

    // Selection and seeking are not edits.
    session.selectClip(video);
    session.requestSeek(50000);
    EXPECT_EQ(session.editOutcomeSequence(), start);

    const auto revision = session.workspace().revision();
    session.splitSelected();
    EXPECT_EQ(session.editOutcomeSequence(), start + 1);
    EXPECT_TRUE(session.lastEditAccepted());
    EXPECT_GT(session.workspace().revision(), revision);

    // A refusal is an attempt too, answered with the header's text.
    EXPECT_FALSE(session.applyCrossfade(video, 0));
    EXPECT_EQ(session.editOutcomeSequence(), start + 2);
    EXPECT_FALSE(session.lastEditAccepted());
    EXPECT_FALSE(session.workspaceError().isEmpty());

    session.undo();
    EXPECT_EQ(session.editOutcomeSequence(), start + 3);
    EXPECT_TRUE(session.lastEditAccepted());
    EXPECT_TRUE(session.workspaceError().isEmpty());
}

TEST(EditWorkspaceOutcome, ImportPendingSpansTheProbeAndARefusedImportIsCounted) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("not-media.mkv"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(4096, 'x'));
    file.close();

    EditSessionAdapter session;
    int pending_changes = 0;
    QObject::connect(&session, &EditSessionAdapter::importPendingChanged, &session, [&] { ++pending_changes; });
    const auto start = session.editOutcomeSequence();
    session.importMediaBatch({QUrl::fromLocalFile(path)}, true);
    EXPECT_TRUE(session.importPending());
    EXPECT_EQ(pending_changes, 1);

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&session, &EditSessionAdapter::importPendingChanged, &loop, &QEventLoop::quit);
    deadline.start(10000);
    if (session.importPending())
        loop.exec();
    ASSERT_FALSE(session.importPending()) << "the probe did not finish";
    EXPECT_EQ(pending_changes, 2);
    EXPECT_EQ(session.editOutcomeSequence(), start + 1);
    EXPECT_FALSE(session.lastEditAccepted());
    EXPECT_FALSE(session.workspaceError().isEmpty());
    EXPECT_TRUE(session.workspace().assets().empty());
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

namespace {
QVariantMap ClipRow(qulonglong id, qint64 start_ms) {
    return {{QStringLiteral("id"), id}, {QStringLiteral("startMs"), start_ms}};
}

std::vector<std::pair<qulonglong, qint64>> ClipModelContent(const EditTimelineClipModel& model) {
    std::vector<std::pair<qulonglong, qint64>> content;
    for (int row = 0; row < model.rowCount(); ++row) {
        const auto map = model.data(model.index(row), EditTimelineClipModel::RowRole).toMap();
        content.emplace_back(map.value(QStringLiteral("id")).toULongLong(),
                             map.value(QStringLiteral("startMs")).toLongLong());
    }
    return content;
}
} // namespace

TEST(EditTimelineClipModel, KeepsRowIdentityAcrossChangesInsteadOfResetting) {
    EnsureApplication();
    EditTimelineClipModel model;
    model.setRows({ClipRow(1, 0), ClipRow(2, 100), ClipRow(3, 200)});
    int resets = 0;
    int inserted = 0;
    int removed = 0;
    int moved = 0;
    std::vector<int> changed;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted,
                     [&inserted](const QModelIndex&, int first, int last) { inserted += last - first + 1; });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved,
                     [&removed](const QModelIndex&, int first, int last) { removed += last - first + 1; });
    QObject::connect(&model, &QAbstractItemModel::rowsMoved, [&moved]() { ++moved; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged,
                     [&changed](const QModelIndex& top, const QModelIndex& bottom) {
                         for (int row = top.row(); row <= bottom.row(); ++row)
                             changed.push_back(row);
                     });

    model.setRows({ClipRow(1, 0), ClipRow(2, 100), ClipRow(3, 200)});
    EXPECT_TRUE(changed.empty());

    model.setRows({ClipRow(1, 0), ClipRow(2, 150), ClipRow(3, 200)});
    EXPECT_EQ(changed, std::vector<int>{1});

    changed.clear();
    model.setRows({ClipRow(3, 200), ClipRow(4, 250), ClipRow(1, 0)});
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(inserted, 1);
    EXPECT_EQ(moved, 1);
    EXPECT_TRUE(changed.empty());
    EXPECT_EQ(ClipModelContent(model), (std::vector<std::pair<qulonglong, qint64>>{{3, 200}, {4, 250}, {1, 0}}));

    model.setRows({});
    EXPECT_EQ(removed, 4);
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(resets, 0);
}

TEST(EditTimelineClipModel, SyncReadsTheSessionWindow) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    EditTimelineClipModel model;
    model.sync(&session, 0, 100000);
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.data(model.index(0), EditTimelineClipModel::RowRole).toMap(),
              session.visibleClips(0, 100000).front().toMap());
    EXPECT_EQ(model.roleNames().value(EditTimelineClipModel::RowRole), QByteArrayLiteral("row"));
    model.sync(nullptr, 0, 100000);
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(EditSessionAdapterPresentation, DeliveryPendingChangesNotifyTheOpenEditPage) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    int notifications = 0;
    QObject::connect(&session, &EditSessionAdapter::clipChanged, [&] { ++notifications; });
    EXPECT_FALSE(session.deliveryPending());
    session.setDeliveryPending(true);
    EXPECT_TRUE(session.deliveryPending());
    EXPECT_EQ(notifications, 1);
    session.setDeliveryPending(true);
    EXPECT_EQ(notifications, 1);
    session.setDeliveryPending(false);
    EXPECT_FALSE(session.deliveryPending());
    EXPECT_EQ(notifications, 2);
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

TEST(EditWorkspaceAdapter, RecordingContextPlacementHonorsDisabledAutoCrossfade) {
    EnsureApplication();
    for (const bool segmented : {false, true}) {
        SCOPED_TRACE(segmented ? "segmented recording" : "single recording");
        EditSessionAdapter session;
        session.setEditContext(MakeContext(10));
        session.autoCrossfade = false;
        EditContext incoming = MakeContext(10);
        if (segmented)
            incoming.segments = {{QStringLiteral("D:/Recordings/segment-0.mkv"), 10, true},
                                 {QStringLiteral("D:/Recordings/segment-1.mkv"), 5, true}};

        session.setEditContext(incoming, 5000);

        ASSERT_TRUE(session.workspaceError().isEmpty());
        ASSERT_EQ(session.workspace().transitions().size(), 2U);
        for (const auto& junction : session.workspace().transitions())
            EXPECT_EQ(junction.kind, edit::TransitionKind::HardCut);
        EXPECT_EQ(session.durationMs(), segmented ? 20000 : 15000);
    }
}

TEST(EditWorkspaceAdapter, OverlappingSegmentBatchChangesNoClipsAndAddsNoUndoCommand) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto original = session.workspace().clips();
    EditContext split = MakeContext(5);
    split.segments = {{QStringLiteral("D:/Recordings/segment-0.mkv"), 2, true},
                      {QStringLiteral("D:/Recordings/segment-1.mkv"), 3, true}};

    session.setEditContext(split, 0);

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

TEST(EditWorkspacePrecision, DragAndVisibleRowsRetainMicrosecondsThroughUndo) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.snapping = false;
    const auto original = session.workspace().clips();
    const auto selected = session.selectedClip();
    session.beginClipDrag(selected);
    session.updateClipDragUs(21);
    ASSERT_TRUE(session.dragValid());
    ASSERT_TRUE(session.commitClipDrag());
    ASSERT_EQ(session.workspace().clips().size(), original.size());
    EXPECT_EQ(session.workspace().clips().front().start, 21);
    EXPECT_EQ(session.durationUs(), 10000021);
    const auto rows = session.visibleClipsUs(21, 22);
    ASSERT_EQ(rows.size(), 2);
    for (const auto& value : rows) {
        const auto row = value.toMap();
        EXPECT_EQ(row.value(QStringLiteral("startUs")).toLongLong(), 21);
        EXPECT_EQ(row.value(QStringLiteral("durationUs")).toLongLong(), 10000000);
    }
    session.undo();
    EXPECT_EQ(session.workspace().clips(), original);
    session.redo();
    EXPECT_EQ(session.workspace().clips().front().start, 21);
}

TEST(EditViewport, SampleScaleUsesLocalCoordinatesAtEightHourOrigin) {
    EditViewportAdapter view;
    view.setUsableWidth(1200);
    view.setTimelineEndUs(36000000000LL);
    view.setVisibleInterval(28800000000LL, 28800012500LL);
    EXPECT_DOUBLE_EQ(view.pixelsPerSecond(), 96000);
    EXPECT_EQ(view.timeAtX(2), 28800000021LL);
    EXPECT_NEAR(view.xAtTime(28800000021LL), 2.016, 0.001);
    EXPECT_EQ(view.endUs(), 28800012500LL);
    const auto anchor = view.timeAtX(743);
    view.zoomAt(48000, 743);
    EXPECT_NEAR(view.xAtTime(anchor), 743, 1);
    view.zoomAt(96000, 743);
    EXPECT_NEAR(view.xAtTime(anchor), 743, 1);
}

TEST(EditViewport, FitStartsAtZeroAndSurvivesResizeAndRapidZoomReversal) {
    EditViewportAdapter view;
    view.setUsableWidth(1000);
    view.setTimelineEndUs(7200000000LL);
    view.fitTimeline();
    EXPECT_EQ(view.startUs(), 0);
    EXPECT_EQ(view.timeAtX(1000), view.timelineEndUs());
    view.setUsableWidth(600);
    EXPECT_EQ(view.timeAtX(600), view.timelineEndUs());
    for (int i = 0; i < 200; ++i) {
        view.zoomAt(96000, 300);
        view.panPixels(1e9);
        view.zoomAt(0.00001, 300);
    }
    EXPECT_EQ(view.startUs(), 0);
    EXPECT_EQ(view.timeAtX(600), view.timelineEndUs());
    view.fitSelection(1000000, 2000000);
    EXPECT_EQ(view.startUs(), 1000000);
    EXPECT_EQ(view.timeAtX(600), 2000000);
    view.restorePreviousView();
    EXPECT_EQ(view.startUs(), 0);
    EXPECT_EQ(view.timeAtX(600), view.timelineEndUs());
}

TEST(EditViewport, EmptyInitializationDoesNotDependOnSetterOrder) {
    EditViewportAdapter first;
    first.setUsableWidth(1200);
    first.setTimelineEndUs(10000000);
    EditViewportAdapter second;
    second.setTimelineEndUs(10000000);
    second.setUsableWidth(1200);
    EXPECT_EQ(first.pixelsPerSecond(), second.pixelsPerSecond());
    EXPECT_LT(first.pixelsPerSecond(), 1000);
}

TEST(EditViewport, BoundaryNoOpsPreservePreviousView) {
    EditViewportAdapter view;
    view.setUsableWidth(600);
    view.setTimelineEndUs(10000000);
    view.fitSelection(1000000, 2000000);
    view.zoomAt(0.001, 300);
    view.zoomAt(0.001, 300);
    view.panPixels(-1000);
    view.fitTimeline();
    view.restorePreviousView();
    EXPECT_EQ(view.startUs(), 1000000);
    EXPECT_EQ(view.endUs(), 2000000);
}

TEST(EditViewport, RejectsUnrepresentableDimensions) {
    EditViewportAdapter view;
    view.setTimelineEndUs(10000000);
    view.setUsableWidth(600);
    view.setUsableWidth((std::numeric_limits<qreal>::max)());
    EXPECT_EQ(view.usableWidth(), 600);
    view.setUsableWidth((std::numeric_limits<qreal>::denorm_min)());
    EXPECT_EQ(view.usableWidth(), 600);
}

TEST(EditWorkspacePrecision, NudgeAndTrimDoNotRoundThroughMillisecondValues) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.nudgeSelectedUs(21);
    EXPECT_EQ(session.workspace().clips().front().start, 21);
    session.nudgeSelectedUs(42, 1);
    EXPECT_EQ(session.workspace().clips().front().start, 63);
    EXPECT_EQ(session.workspace().clips().front().source_in, 42);
    EXPECT_EQ(session.workspace().clips().front().source_out, 10000000);
}

TEST(EditWorkspacePrecision, ContextDurationNotifiesWithinTheSameMillisecond) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    int notifications = 0;
    QObject::connect(&session, &EditSessionAdapter::durationChanged, [&] { ++notifications; });
    const auto before = session.durationUs();
    session.setEditContext(MakeContext(0.000021));
    ASSERT_NE(session.durationUs(), before);
    EXPECT_GT(notifications, 0);
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

TEST(EditWorkspaceAdapter, SavedStandaloneClipRemainsImportableAndEditableWhenCreationIsUnavailable) {
    EnsureApplication();
    EXPECT_FALSE(product::kRecordingClipsAvailable);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const auto saved = directory.filePath(QStringLiteral("saved.clip.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, saved));
    EditSessionAdapter session;
    QEventLoop loop;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    session.importMedia(QUrl::fromLocalFile(saved), true);
    loop.exec();
    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    ASSERT_EQ(session.workspace().assets().size(), 1u);
    EXPECT_EQ(session.workspace().assets().front().path, saved.toStdWString());
    ASSERT_EQ(session.workspace().clips().size(), 1u);
    const auto clip = session.workspace().clips().front();
    ASSERT_GT(clip.duration(), 200'000);
    session.trimClip(clip.id, 100, (clip.source_out / 1000) - 100);
    EXPECT_TRUE(session.workspaceError().isEmpty());
    EXPECT_LT(session.workspace().clips().front().duration(), clip.duration());
    session.undo();
    EXPECT_EQ(session.workspace().clips().front(), clip);
    EXPECT_TRUE(QFileInfo::exists(saved));
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
        if (session.workspace().clips().size() == 4)
            loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);

    session.setEditContext(split, 3000);
    loop.exec();

    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    ASSERT_EQ(session.workspace().assets().size(), 3U);
    // The fixture has no audio stream, so each probed segment adds one video clip.
    ASSERT_EQ(session.workspace().clips().size(), 4U);
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

TEST(EditWorkspaceAdapter, SyntheticImportDescriptorRetainsVerifiedAudioIdentityAuthority) {
    engine::EditMediaMetadata metadata;
    metadata.content_identity_valid = true;
    metadata.duration_us = 2'000'000;
    metadata.content_sha256.fill(23);
    engine::EditSourceStream stream;
    stream.kind = engine::TimelineStreamKind::Audio;
    stream.container_index = 4;
    stream.codec.channels = 2;
    stream.stream_identity = engine::AudioTrackId{{7, 8, 9}};
    metadata.streams.push_back(stream);
    for (const bool verified : {false, true}) {
        metadata.audio_metadata.status =
            verified ? engine::AudioMetadataStatus::Verified : engine::AudioMetadataStatus::Missing;
        const auto imported = edit::DescribeSourceImport(metadata);
        ASSERT_TRUE(imported.descriptor);
        EXPECT_EQ(imported.descriptor->audio_identities_verified, verified);
        ASSERT_EQ(imported.descriptor->streams.size(), 1u);
        EXPECT_EQ(imported.descriptor->streams.front().ref.stream_identity, *stream.stream_identity);
    }
}

TEST(EditWorkspaceAdapter, RealReimportReusesWaveformCacheWithoutChangingSemanticStreamIdentities) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("source.mkv"));
    ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("red.mkv")), path));
    const auto metadata = engine::ProbeEditMedia(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    ASSERT_NE(metadata.audio_metadata.status, engine::AudioMetadataStatus::Verified);
    const auto wait_for = [](const auto& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        return predicate();
    };
    EditSessionAdapter session;
    WaveformCacheService::Config config;
    config.root = std::filesystem::path(directory.filePath(QStringLiteral("waveforms")).toStdWString());
    QString first_key;
    engine::AudioTrackId first_semantic_identity;
    std::vector<engine::WaveformPeak> first_peaks;
    for (int cycle = 0; cycle < 2; ++cycle) {
        SCOPED_TRACE(cycle);
        session.importMedia(QUrl::fromLocalFile(path), true);
        ASSERT_TRUE(wait_for(
            [&]() { return session.workspace().assets().size() == 1 || !session.workspaceError().isEmpty(); }));
        ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
        EditTimelineAdapter timeline;
        timeline.configureWaveformCache(config);
        auto* waveforms = qobject_cast<WaveformCacheService*>(timeline.waveformService());
        ASSERT_NE(waveforms, nullptr);
        QVariantMap audio;
        for (const auto& row : session.visibleClips(0, session.durationMs()))
            if (!row.toMap().value(QStringLiteral("video")).toBool()) {
                ASSERT_TRUE(audio.isEmpty());
                audio = row.toMap();
            }
        ASSERT_FALSE(audio.isEmpty());
        const auto* clip = session.workspace().clip(audio.value(QStringLiteral("id")).toULongLong());
        ASSERT_NE(clip, nullptr);
        ASSERT_TRUE(clip->stream);
        const auto semantic_identity = clip->stream->stream_identity;
        const auto key = timeline.waveformSource(audio.value(QStringLiteral("sourceIdentity")).toString(),
                                                 audio.value(QStringLiteral("path")).toString(),
                                                 audio.value(QStringLiteral("containerIndex")).toInt(),
                                                 audio.value(QStringLiteral("streamIdentity")).toString(),
                                                 audio.value(QStringLiteral("sourceEndUs")).toLongLong());
        ASSERT_FALSE(key.isEmpty());
        ASSERT_TRUE(wait_for([&]() { return waveforms->state(key) == WaveformCacheService::State::Ready; }));
        std::vector<engine::WaveformPeak> peaks;
        ASSERT_TRUE(wait_for([&]() { return waveforms->readBlock(key, 0, 0, peaks) > 0; }));
        EXPECT_EQ(clip->stream->stream_identity, semantic_identity);
        if (cycle == 0) {
            first_key = key;
            first_semantic_identity = semantic_identity;
            first_peaks = peaks;
            EXPECT_EQ(waveforms->decoderOpensForTest(), 1u);
            EXPECT_GT(waveforms->decodeCallsForTest(), 0u);
        } else {
            EXPECT_NE(semantic_identity, first_semantic_identity);
            EXPECT_EQ(key, first_key);
            EXPECT_EQ(peaks, first_peaks);
            EXPECT_EQ(waveforms->decoderOpensForTest(), 0u);
            EXPECT_EQ(waveforms->decodeCallsForTest(), 0u);
            EXPECT_EQ(waveforms->decoderSeeksForTest(), 0u);
        }
        session.close();
        EXPECT_TRUE(session.workspace().clips().empty());
        EXPECT_TRUE(session.workspace().assets().empty());
    }
    const auto after = engine::ProbeEditMedia(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(after.error.empty()) << after.error;
    EXPECT_EQ(after.content_sha256, metadata.content_sha256);
}

TEST(EditTimelineAdapterTiles, AShrunkWindowKeepsTheTileIdentitiesItStillShows) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    const auto path = QDir(fixtures).filePath(QStringLiteral("red.mkv"));
    const auto metadata = engine::ProbeEditMedia(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    const auto identity = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(metadata.content_sha256.data()), metadata.content_sha256.size())
            .toHex());
    EditTimelineAdapter timeline;
    ASSERT_TRUE(timeline.registerThumbnailSource(identity, path, metadata.width, metadata.height, metadata.fps_num,
                                                 metadata.fps_den));

    // The same scale before and after, the way a trim or split shrinks a clip.
    const auto full = timeline.thumbnailTiles(identity, 0, 2'000'000, 800, 44);
    const auto left = timeline.thumbnailTiles(identity, 0, 1'000'000, 400, 44);
    ASSERT_GE(full.size(), 4);
    ASSERT_GE(left.size(), 2);
    QHash<qulonglong, QString> full_sources;
    for (const auto& tile : full) {
        const auto row = tile.toMap();
        const auto id = row.value(QStringLiteral("id")).toULongLong();
        EXPECT_FALSE(full_sources.contains(id)) << "tile ids must be unique within one strip";
        full_sources.insert(id, row.value(QStringLiteral("source")).toString());
    }
    for (const auto& tile : left) {
        const auto row = tile.toMap();
        const auto id = row.value(QStringLiteral("id")).toULongLong();
        ASSERT_TRUE(full_sources.contains(id));
        EXPECT_EQ(full_sources.value(id), row.value(QStringLiteral("source")).toString());
    }

    EditTimelineClipModel model;
    model.setRows(full);
    int resets = 0;
    int inserted = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted,
                     [&inserted](const QModelIndex&, int first, int last) { inserted += last - first + 1; });
    model.setRows(left);
    EXPECT_EQ(resets, 0);
    EXPECT_EQ(inserted, 0);
    EXPECT_EQ(model.rowCount(), left.size());
}

TEST(EditWorkspaceAdapter, ExportReviewListsEachSourceStreamOnceWithAName) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    QTemporaryDir sources;
    QTemporaryDir output;
    ASSERT_TRUE(sources.isValid());
    ASSERT_TRUE(output.isValid());
    const auto wait_for = [](const auto& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        return predicate();
    };
    const auto path = sources.filePath(QStringLiteral("red.mkv"));
    ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("red.mkv")), path));
    EditSessionAdapter session;
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(
        wait_for([&]() { return session.workspace().assets().size() == 1 || !session.workspaceError().isEmpty(); }));
    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    const auto tracks = session.workspace().tracks();
    const auto is_audio = [&](const auto& clip) {
        return std::any_of(tracks.begin(), tracks.end(), [&](const auto& track) {
            return track.id == clip.track && track.type == edit::TrackType::Audio;
        });
    };
    const auto imported = session.workspace().clips();
    const auto video =
        std::find_if(imported.begin(), imported.end(), [&](const auto& clip) { return !is_audio(clip); });
    ASSERT_NE(video, imported.end());
    const auto video_id = video->id;
    for (const qint64 at : {500, 1000, 1500}) {
        session.selectClip(video_id);
        session.requestSeek(at);
        session.splitSelected();
    }
    const auto edited = session.workspace().clips();
    const auto audio_clips =
        std::count_if(edited.begin(), edited.end(), [&](const auto& clip) { return is_audio(clip); });
    ASSERT_GE(audio_clips, 4);

    EditExportAdapter exporter;
    exporter.setSession(&session);
    exporter.setRenderConfigProvider([] {
        engine::RecorderConfig config;
        config.video_codec = engine::VideoCodec::H264;
        return config;
    });
    exporter.setOutputDirectoryProvider([&] { return output.path(); });
    exporter.startExport();
    ASSERT_TRUE(wait_for([&]() { return exporter.state() != EditExportAdapter::Preparing; }));
    ASSERT_EQ(exporter.state(), EditExportAdapter::Review) << exporter.errorText().toStdString();
    const auto selected = exporter.reviewOutcome().value(QStringLiteral("selectedTracks")).toList();
    ASSERT_EQ(selected.size(), 1);
    EXPECT_FALSE(selected.front().toMap().value(QStringLiteral("name")).toString().isEmpty());
    exporter.reset();
}

TEST(EditWorkspaceAdapter, RealEditingHistoryAndBlankTimeReachScopedExportsWithoutChangingSources) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    QTemporaryDir sources;
    QTemporaryDir output;
    ASSERT_TRUE(sources.isValid());
    ASSERT_TRUE(output.isValid());
    const auto wait_for = [](const auto& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        return predicate();
    };
    QList<QUrl> urls;
    QJsonArray source_manifest;
    std::vector<engine::EditMediaMetadata> source_metadata;
    for (const auto& name : {QStringLiteral("red.mkv"), QStringLiteral("blue.mkv")}) {
        const auto path = sources.filePath(name);
        ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(name), path));
        auto metadata = engine::ProbeEditMedia(std::filesystem::path(path.toStdWString()));
        ASSERT_TRUE(metadata.error.empty()) << metadata.error;
        ASSERT_EQ(metadata.duration_us, 2'000'000);
        source_manifest.append(
            QJsonObject{{QStringLiteral("name"), name},
                        {QStringLiteral("path"), QDir(fixtures).absoluteFilePath(name)},
                        {QStringLiteral("sha256"),
                         QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(metadata.content_sha256.data()),
                                                        metadata.content_sha256.size())
                                                 .toHex())}});
        source_metadata.push_back(std::move(metadata));
        urls.push_back(QUrl::fromLocalFile(path));
    }
    EditSessionAdapter session;
    session.autoCrossfade = false;
    session.importMedia(urls.front(), true);
    ASSERT_TRUE(
        wait_for([&]() { return session.workspace().assets().size() == 1 || !session.workspaceError().isEmpty(); }));
    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    ASSERT_EQ(session.workspace().clips().size(), 2u);
    ASSERT_EQ(session.durationMs(), 2000);
    const auto red_asset = session.workspace().assets()[0].id;
    const auto red = session.workspace().clips()[0].id;
    const auto red_track = session.workspace().clip(red)->track;
    EditExportAdapter exporter;
    exporter.setSession(&session);
    exporter.setRenderConfigProvider([] {
        engine::RecorderConfig config;
        config.video_codec = engine::VideoCodec::H264;
        return config;
    });
    QString evidence;
    if (const auto root = qEnvironmentVariable("EXOSNAP_EDIT_QUALIFICATION_OUTPUT"); !root.isEmpty()) {
        ASSERT_TRUE(QDir(root).exists());
        evidence = QDir(root).filePath(QStringLiteral("adapter-workflow-") + QFileInfo(output.path()).fileName());
        ASSERT_TRUE(QDir().mkpath(evidence));
    }
    QJsonArray exports;
    const auto export_scope = [&](const QString& scope, const QString& container, qint64 in_us, qint64 out_us,
                                  const QJsonArray& windows) {
        const auto destination =
            output.filePath(scope + QLatin1Char('-') + container + QLatin1Char('-') + exporter.modeKey() +
                            QLatin1Char('-') + QString::number(out_us - in_us));
        ASSERT_TRUE(QDir().mkpath(destination));
        exporter.reset();
        exporter.setSpanKey(scope);
        exporter.setContainerKey(container);
        exporter.setOutputDirectoryProvider([destination] { return destination; });
        exporter.startExport();
        ASSERT_TRUE(wait_for([&]() { return exporter.state() != EditExportAdapter::Preparing; }));
        ASSERT_EQ(exporter.state(), EditExportAdapter::Review) << exporter.errorText().toStdString();
        EXPECT_FALSE(exporter.reviewOutcome().value(QStringLiteral("videoOnly")).toBool());
        const auto reviewed = exporter.reviewOutcome();
        if (session.workspace().assets().size() == 1) {
            EXPECT_EQ(reviewed.value(QStringLiteral("path")).toString(),
                      exporter.modeKey() == QStringLiteral("originalStreams") ? QStringLiteral("Copy")
                                                                              : QStringLiteral("Render"));
            if (exporter.modeKey() == QStringLiteral("finishedStereo")) {
                const auto tracks = reviewed.value(QStringLiteral("outputAudioTracks")).toList();
                ASSERT_EQ(tracks.size(), 1);
                EXPECT_EQ(tracks.front().toMap().value(QStringLiteral("codec")).toString(), QStringLiteral("PCM24"));
            }
        }
        exporter.confirmPreparedExport();
        ASSERT_TRUE(wait_for([&]() { return !exporter.running(); }));
        ASSERT_EQ(exporter.state(), EditExportAdapter::Done) << exporter.errorText().toStdString();
        EXPECT_FALSE(session.exportRunning());
        const auto media = engine::ProbeEditMedia(std::filesystem::path(exporter.outputPath().toStdWString()));
        ASSERT_TRUE(media.error.empty()) << media.error;
        EXPECT_NEAR(static_cast<double>(media.duration_us), static_cast<double>(out_us - in_us), 50'000);
        EXPECT_TRUE(media.has_audio);
        if (session.workspace().assets().size() == 1 && exporter.modeKey() == QStringLiteral("finishedStereo")) {
            ASSERT_EQ(media.audio_metadata.status, engine::AudioMetadataStatus::Verified);
            EditSessionAdapter recorded;
            recorded.importMedia(QUrl::fromLocalFile(exporter.outputPath()), true);
            ASSERT_TRUE(wait_for(
                [&]() { return recorded.workspace().assets().size() == 1 || !recorded.workspaceError().isEmpty(); }));
            ASSERT_TRUE(recorded.workspaceError().isEmpty()) << recorded.workspaceError().toStdString();
            int audio_rows = 0;
            for (const auto& value : recorded.visibleClips(0, recorded.durationMs())) {
                const auto row = value.toMap();
                if (row.value(QStringLiteral("video")).toBool())
                    continue;
                ++audio_rows;
                const auto* clip = recorded.workspace().clip(row.value(QStringLiteral("id")).toULongLong());
                ASSERT_NE(clip, nullptr);
                ASSERT_TRUE(clip->stream);
                const auto semantic =
                    QByteArray(reinterpret_cast<const char*>(clip->stream->stream_identity.bytes.data()), 16).toHex();
                EXPECT_NE(semantic, QByteArray(32, '0'));
                EXPECT_EQ(row.value(QStringLiteral("streamIdentity")).toString().toLatin1(), semantic);
            }
            EXPECT_EQ(audio_rows, 1);
        }
        EXPECT_FALSE(QFileInfo::exists(exporter.outputPath() + QStringLiteral(".tmp")));
        const auto filename = scope + QLatin1Char('-') + exporter.modeKey() + QLatin1Char('-') +
                              QString::number(out_us - in_us) + QLatin1Char('.') + container;
        if (!evidence.isEmpty())
            ASSERT_TRUE(QFile::copy(exporter.outputPath(), QDir(evidence).filePath(filename)));
        exports.append(
            QJsonObject{{QStringLiteral("file"), filename},
                        {QStringLiteral("path"),
                         evidence.isEmpty() ? exporter.outputPath() : QDir(evidence).absoluteFilePath(filename)},
                        {QStringLiteral("scope"), scope},
                        {QStringLiteral("mode"), exporter.modeKey()},
                        {QStringLiteral("review"), QJsonObject::fromVariantMap(reviewed)},
                        {QStringLiteral("expectedDurationUs"), out_us - in_us},
                        {QStringLiteral("timelineInUs"), in_us},
                        {QStringLiteral("timelineOutUs"), out_us},
                        {QStringLiteral("contentWindows"), windows}});
    };
    const auto window = [](qint64 in_us, qint64 out_us, const char* content) {
        return QJsonObject{{QStringLiteral("inUs"), in_us},
                           {QStringLiteral("outUs"), out_us},
                           {QStringLiteral("content"), QString::fromLatin1(content)},
                           {QStringLiteral("audioLevel"), std::string_view(content) == "red"    ? 0.25
                                                          : std::string_view(content) == "blue" ? -0.25
                                                                                                : 0.0}};
    };
    session.selectClip(red);
    const auto copy_assessment = engine::AssessTimelineExport(
        {{std::filesystem::path(urls.front().toLocalFile().toStdWString()), 0, 2'000'000, 0}}, false);
    ASSERT_EQ(copy_assessment.eligibility, engine::TimelineExportEligibility::StreamCopy) << copy_assessment.reason;
    exporter.setModeKey(QStringLiteral("originalStreams"));
    export_scope(QStringLiteral("entire"), QStringLiteral("mkv"), 0, 2'000'000, {window(0, 2'000'000, "red")});
    ASSERT_FALSE(HasFatalFailure());
    exporter.setModeKey(QStringLiteral("finishedStereo"));
    export_scope(QStringLiteral("entire"), QStringLiteral("mkv"), 0, 2'000'000, {window(0, 2'000'000, "red")});
    ASSERT_FALSE(HasFatalFailure());
    session.importMedia(urls.back(), true);
    ASSERT_TRUE(
        wait_for([&]() { return session.workspace().assets().size() == 2 || !session.workspaceError().isEmpty(); }));
    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    ASSERT_EQ(session.workspace().clips().size(), 4u);
    ASSERT_EQ(session.durationMs(), 4000);
    const auto imported = session.workspace().snapshot();
    const auto blue_asset = session.workspace().assets()[1].id;
    const auto blue =
        std::find_if(session.workspace().clips().begin(), session.workspace().clips().end(), [&](const auto& clip) {
            return clip.asset == blue_asset && clip.stream && clip.stream->kind == engine::TimelineStreamKind::Video;
        });
    ASSERT_NE(blue, session.workspace().clips().end());
    const auto blue_id = blue->id;
    exporter.setModeKey(QStringLiteral("finishedStereo"));
    WaveformCacheService::Config waveform_config;
    waveform_config.root = std::filesystem::path(output.filePath(QStringLiteral("waveforms")).toStdWString());
    auto waveforms = std::make_unique<WaveformCacheService>(waveform_config, WaveformCacheService::DecoderFactory{});
    std::map<QString, std::vector<engine::WaveformPeak>> original_peaks;
    const auto request_waveforms = [&](WaveformCacheService& service) {
        for (const auto& value : session.visibleClips(0, session.durationMs())) {
            const auto row = value.toMap();
            if (row.value(QStringLiteral("video")).toBool())
                continue;
            engine::WaveformSourceKey source;
            const auto identity =
                QByteArray::fromHex(row.value(QStringLiteral("sourceIdentity")).toString().toLatin1());
            const auto stream = QByteArray::fromHex(row.value(QStringLiteral("streamIdentity")).toString().toLatin1());
            ASSERT_EQ(identity.size(), source.content_sha256.size());
            ASSERT_EQ(stream.size(), source.stream_identity.size());
            std::copy(identity.begin(), identity.end(), source.content_sha256.begin());
            std::copy(stream.begin(), stream.end(), source.stream_identity.begin());
            source.container_index = row.value(QStringLiteral("containerIndex")).toUInt();
            const auto key = service.request(source, row.value(QStringLiteral("path")).toString(),
                                             row.value(QStringLiteral("sourceEndUs")).toLongLong());
            ASSERT_TRUE(wait_for([&]() { return service.state(key) == WaveformCacheService::State::Ready; }));
            std::vector<engine::WaveformPeak> peaks;
            ASSERT_TRUE(wait_for([&]() { return service.readBlock(key, 0, 0, peaks) > 0; }));
            if (const auto found = original_peaks.find(key); found != original_peaks.end())
                EXPECT_EQ(peaks, found->second);
            else
                original_peaks.emplace(key, std::move(peaks));
        }
    };
    request_waveforms(*waveforms);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(original_peaks.size(), 2u);
    ASSERT_EQ(waveforms->decoderOpensForTest(), 2u);
    const auto decode_calls = waveforms->decodeCallsForTest();
    ASSERT_GT(decode_calls, 0u);
    session.trimClip(red, 500, 1500);
    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    const auto trimmed = session.workspace().snapshot();
    session.selectClip(red);
    session.requestSeek(1000);
    session.splitSelected();
    ASSERT_EQ(session.workspace().clips().size(), 6u);
    const auto split = session.workspace().snapshot();
    session.deleteSelected(false);
    ASSERT_EQ(session.workspace().clips().size(), 4u);
    const auto deleted = session.workspace().snapshot();
    session.undo();
    EXPECT_EQ(session.workspace().clips(), split->clips);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), trimmed->clips);
    session.redo();
    session.redo();
    EXPECT_EQ(session.workspace().clips(), deleted->clips);
    EXPECT_EQ(imported->clips.size(), 4u);
    EXPECT_EQ(imported->clips.front().source_in, 0);
    EXPECT_EQ(imported->clips.front().source_out, 2'000'000);
    const auto red_tail = std::find_if(session.workspace().clips().begin(), session.workspace().clips().end(),
                                       [&](const auto& clip) { return clip.asset == red_asset; });
    ASSERT_NE(red_tail, session.workspace().clips().end());
    EXPECT_EQ(red_tail->source_in, 1'000'000);
    EXPECT_EQ(red_tail->source_out, 1'500'000);
    session.moveClip(blue_id, 2500, false);
    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    session.setFixedTimelineEnd(true, 6000);
    ASSERT_TRUE(session.fixedTimelineEnd());
    ASSERT_EQ(session.durationMs(), 6000);
    session.requestSeek(0);
    session.selectGap(red_track, 500);
    ASSERT_EQ(session.selectedGap().value(QStringLiteral("endMs")).toLongLong(), 1000);
    session.copyBlankDuration();
    session.pasteSpacer();
    ASSERT_EQ(session.spacers().size(), 1);
    const auto blank = session.workspace().snapshot();
    session.undo();
    EXPECT_TRUE(session.spacers().isEmpty());
    session.redo();
    EXPECT_EQ(session.workspace().snapshot()->spacers, blank->spacers);
    EXPECT_EQ(session.workspace().clips(), blank->clips);
    EXPECT_EQ(session.durationMs(), 6000);
    request_waveforms(*waveforms);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(original_peaks.size(), 2u);
    EXPECT_EQ(waveforms->decoderOpensForTest(), 2u);
    EXPECT_EQ(waveforms->decodeCallsForTest(), decode_calls);
    waveforms.reset();
    WaveformCacheService reopened(waveform_config, {});
    request_waveforms(reopened);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(reopened.decoderOpensForTest(), 0u);
    EXPECT_EQ(reopened.decodeCallsForTest(), 0u);
    EXPECT_EQ(reopened.decoderSeeksForTest(), 0u);
    const auto timeline = session.workspace().timeline();
    for (const auto& [time_us, asset, source_us] :
         {std::tuple{250'000ll, edit::Id{0}, 0ll}, std::tuple{1'250'000ll, red_asset, 1'250'000ll},
          std::tuple{2'000'000ll, edit::Id{0}, 0ll}, std::tuple{3'000'000ll, blue_asset, 500'000ll},
          std::tuple{5'000'000ll, edit::Id{0}, 0ll}}) {
        const auto evaluation = engine::EvaluateTimeline(timeline, time_us);
        ASSERT_TRUE(evaluation.error.empty()) << evaluation.error;
        if (!asset)
            EXPECT_TRUE(evaluation.video.empty());
        else {
            ASSERT_EQ(evaluation.video.size(), 1u);
            EXPECT_EQ(evaluation.video.front().asset, asset);
            EXPECT_EQ(evaluation.video.front().source_us, source_us);
        }
    }
    session.selectClip(blue_id);
    session.requestSeekUs(750'000);
    session.setSelectionBoundary(false);
    session.requestSeekUs(3'000'000);
    session.setSelectionBoundary(true);
    exporter.setTimeSelectionUs(session.timeSelectionInUs(), session.timeSelectionOutUs());
    const QJsonArray full{window(0, 1'000'000, "black"), window(1'000'000, 1'500'000, "red"),
                          window(1'500'000, 2'500'000, "black"), window(2'500'000, 4'500'000, "blue"),
                          window(4'500'000, 6'000'000, "black")};
    export_scope(QStringLiteral("entire"), QStringLiteral("mkv"), 0, 6'000'000, full);
    ASSERT_FALSE(HasFatalFailure());
    export_scope(
        QStringLiteral("occupied"), QStringLiteral("mkv"), 1'000'000, 4'500'000,
        {window(0, 500'000, "red"), window(500'000, 1'500'000, "black"), window(1'500'000, 3'500'000, "blue")});
    ASSERT_FALSE(HasFatalFailure());
    export_scope(QStringLiteral("selected"), QStringLiteral("mp4"), 2'500'000, 4'500'000,
                 {window(0, 2'000'000, "blue")});
    ASSERT_FALSE(HasFatalFailure());
    export_scope(QStringLiteral("time"), QStringLiteral("mkv"), 750'000, 3'000'000,
                 {window(0, 250'000, "black"), window(250'000, 750'000, "red"), window(750'000, 1'750'000, "black"),
                  window(1'750'000, 2'250'000, "blue")});
    ASSERT_FALSE(HasFatalFailure());
    export_scope(QStringLiteral("result"), QStringLiteral("mkv"), 0, 6'000'000, full);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(session.workspace().clips(), blank->clips);
    EXPECT_EQ(session.workspace().snapshot()->spacers, blank->spacers);
    for (qsizetype i = 0; i < urls.size(); ++i) {
        const auto after = engine::ProbeEditMedia(std::filesystem::path(urls[i].toLocalFile().toStdWString()));
        ASSERT_TRUE(after.error.empty()) << after.error;
        EXPECT_EQ(after.content_sha256, source_metadata[i].content_sha256);
    }
    if (!evidence.isEmpty()) {
        QFile manifest(QDir(evidence).filePath(QStringLiteral("manifest.json")));
        ASSERT_TRUE(manifest.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("sources"), source_manifest},
                                                     {QStringLiteral("exports"), exports}})
                               .toJson();
        ASSERT_EQ(manifest.write(bytes), bytes.size());
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

class EditExportSourceLease : public ::testing::TestWithParam<const char*> {};

TEST_P(EditExportSourceLease, AllFrozenSourcesRemainImmutableBetweenChecksAndReleaseOnCancellation) {
    EnsureApplication();
    const bool render = std::string_view(GetParam()) == "render";
    const bool selected_only = std::string_view(GetParam()) == "selected";
    const bool use_render_fixture = render || selected_only;
    const auto render_fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (use_render_fixture && render_fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    QTemporaryDir sources;
    QTemporaryDir output;
    ASSERT_TRUE(sources.isValid());
    ASSERT_TRUE(output.isValid());
    const QString fixture =
        use_render_fixture
            ? QDir(render_fixtures).filePath(QStringLiteral("red.mkv"))
            : QFileInfo(QString::fromUtf8(__FILE__))
                  .dir()
                  .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString first = sources.filePath(QStringLiteral("first.mp4"));
    const QString second = sources.filePath(QStringLiteral("second.mp4"));
    const QString replacement = sources.filePath(QStringLiteral("replacement.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, first));
    ASSERT_TRUE(QFile::copy(fixture, second));
    ASSERT_TRUE(QFile::copy(fixture, replacement));
    QFile replacement_file(replacement);
    ASSERT_TRUE(replacement_file.open(QIODevice::Append));
    ASSERT_EQ(replacement_file.write("replacement", 11), 11);
    replacement_file.close();
    QFile original(first);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    const auto original_bytes = original.readAll();
    original.close();
    EditSessionAdapter session;
    QEventLoop imported;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &imported, &QEventLoop::quit);
    QTimer::singleShot(10000, &imported, &QEventLoop::quit);
    session.importMediaBatch({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)}, true);
    imported.exec();
    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    ASSERT_EQ(session.workspace().assets().size(), 2u);
    if (use_render_fixture) {
        for (const auto& asset : session.workspace().assets()) {
            engine::EditPlayerEngine player;
            std::string error;
            ASSERT_TRUE(player.Open(std::filesystem::path(asset.path), error)) << error;
            const auto metadata = player.AudioMetadata();
            engine::MonitorSelectionSnapshot selection{
                metadata.selection_media_id, {1}, {}, true, metadata.unverified_asset_binding};
            for (const auto& track : metadata.actual_tracks)
                selection.tracks.push_back({track.track_id, 1.0f, false});
            session.setAssetMonitorSelection(asset.id, std::move(selection));
        }
    }
    if (render)
        ASSERT_TRUE(session.applyCrossfade(session.workspace().clips().front().id));
    if (selected_only) {
        session.selectClip(session.workspace().clips().front().id);
        ASSERT_TRUE(QFile::remove(second));
    }
    auto exporter = std::make_unique<EditExportAdapter>();
    exporter->setSession(&session);
    if (selected_only)
        exporter->setSpanKey(QStringLiteral("selected"));
    else if (!render)
        exporter->setModeKey(QString::fromUtf8(GetParam()));
    exporter->setOutputDirectoryProvider([&] { return output.path(); });
    exporter->startExport();
    QEventLoop review;
    QObject::connect(exporter.get(), &EditExportAdapter::stateChanged, &review, [&] {
        if (exporter->state() != EditExportAdapter::Preparing)
            review.quit();
    });
    QTimer::singleShot(10000, &review, &QEventLoop::quit);
    if (exporter->state() == EditExportAdapter::Preparing)
        review.exec();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Review) << exporter->errorText().toStdString();
    EXPECT_EQ(exporter->reviewOutcome().value(QStringLiteral("path")).toString(),
              use_render_fixture ? QStringLiteral("Render") : QStringLiteral("Copy"));
    struct VerificationGate {
        QSemaphore reached;
        QSemaphore resume;
        std::atomic<bool> timed_out{false};
    };
    const auto gate = std::make_shared<VerificationGate>();
    EditExportAdapterTestPeer::observeSourceVerification(
        *exporter, [gate, first_id = session.workspace().assets().front().id](uint64_t id) {
            if (id == first_id) {
                gate->reached.release();
                gate->timed_out.store(!gate->resume.tryAcquire(1, 10000));
            }
        });
    const auto cleanup = qScopeGuard([&] {
        if (exporter)
            exporter->cancel();
        gate->resume.release();
        exporter.reset();
    });
    exporter->confirmPreparedExport();
    ASSERT_TRUE(gate->reached.tryAcquire(1, 10000));
    const bool replaced = MoveFileExW(reinterpret_cast<const wchar_t*>(replacement.utf16()),
                                      reinterpret_cast<const wchar_t*>(first.utf16()), MOVEFILE_REPLACE_EXISTING);
    const auto rename_error = GetLastError();
    EXPECT_FALSE(replaced) << "An already verified source must not be replaceable before another source is checked.";
    if (!replaced)
        EXPECT_TRUE(rename_error == ERROR_SHARING_VIOLATION || rename_error == ERROR_ACCESS_DENIED) << rename_error;
    for (const auto& path : {first, second}) {
        if (selected_only && path == second)
            continue;
        const HANDLE writer = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto write_error = GetLastError();
        if (writer != INVALID_HANDLE_VALUE)
            CloseHandle(writer);
        EXPECT_EQ(writer, INVALID_HANDLE_VALUE) << path.toStdString();
        if (writer == INVALID_HANDLE_VALUE)
            EXPECT_EQ(write_error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    }
    EXPECT_TRUE(QDir(output.path()).entryList(QDir::Files).isEmpty());
    exporter->cancel();
    gate->resume.release();
    exporter.reset();
    EXPECT_FALSE(gate->timed_out.load());
    EXPECT_TRUE(QDir(output.path()).entryList(QDir::Files).isEmpty());
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_TRUE(original.readAll() == original_bytes);
    original.close();
    for (const auto& path : {first, second}) {
        if (selected_only && path == second)
            continue;
        QFile writable(path);
        EXPECT_TRUE(writable.open(QIODevice::ReadWrite)) << path.toStdString();
    }
}

INSTANTIATE_TEST_SUITE_P(FinishedAndOriginal, EditExportSourceLease,
                         ::testing::Values("finishedStereo", "originalStreams", "selected", "render"));

TEST(EditExportAdapterRun, CancellingPreparationAndDestroyingAdapterLeavesNoOutput) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    EditSessionAdapter session;
    const QString fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    QEventLoop imported;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &imported, &QEventLoop::quit);
    QTimer::singleShot(10000, &imported, &QEventLoop::quit);
    session.importMedia(QUrl::fromLocalFile(fixture), true);
    imported.exec();
    ASSERT_EQ(session.workspace().clips().size(), 1u);
    auto exporter = std::make_unique<EditExportAdapter>();
    exporter->setSession(&session);
    exporter->setOutputDirectoryProvider([&] { return directory.path(); });
    exporter->startExport();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Preparing);
    exporter->cancel();
    EXPECT_EQ(exporter->state(), EditExportAdapter::Cancelling);
    exporter.reset();
    EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Files).isEmpty());
}

TEST(EditExportAdapterRun, DestructionCancelsAnActiveSourceProbeBeforeTheWholeHashCompletes) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString source_path = QDir(directory.path()).filePath(QStringLiteral("large-source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, source_path));
    {
        const HANDLE file = CreateFileW(reinterpret_cast<const wchar_t*>(source_path.utf16()), GENERIC_WRITE,
                                        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(file, INVALID_HANDLE_VALUE);
        DWORD returned = 0;
        const bool sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
        LARGE_INTEGER end{};
        end.QuadPart = 2LL * 1024 * 1024 * 1024;
        const bool extended = sparse && SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
        CloseHandle(file);
        ASSERT_TRUE(extended);
    }
    QTemporaryDir output;
    ASSERT_TRUE(output.isValid());
    {
        EditSessionAdapter session;
        QEventLoop imported;
        QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &imported, &QEventLoop::quit);
        QTimer::singleShot(60000, &imported, &QEventLoop::quit);
        session.importMedia(QUrl::fromLocalFile(source_path), true);
        imported.exec();
        ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
        ASSERT_EQ(session.workspace().clips().size(), 1u);
        auto exporter = std::make_unique<EditExportAdapter>();
        exporter->setSession(&session);
        exporter->setOutputDirectoryProvider([&] { return output.path(); });
        IO_COUNTERS before{}, current{};
        ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &before));
        exporter->startExport();
        ASSERT_EQ(exporter->state(), EditExportAdapter::Preparing);
        QPointer<QThread> worker = EditExportAdapterTestPeer::worker(*exporter);
        ASSERT_NE(worker, nullptr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &current));
            if (current.ReadTransferCount - before.ReadTransferCount >= 4 * 1024 * 1024)
                break;
            std::this_thread::yield();
        } while (worker->isRunning() && std::chrono::steady_clock::now() < deadline);
        ASSERT_GE(current.ReadTransferCount - before.ReadTransferCount, 4u * 1024 * 1024);
        ASSERT_TRUE(worker->isRunning());
        const auto cancelled = std::chrono::steady_clock::now();
        exporter->cancel();
        exporter.reset();
        EXPECT_LT(std::chrono::steady_clock::now() - cancelled, std::chrono::seconds(1));
        EXPECT_TRUE(worker.isNull());
        EXPECT_TRUE(QDir(output.path()).entryList(QDir::Files).isEmpty());
    }
    QFile writable(source_path);
    EXPECT_TRUE(writable.open(QIODevice::WriteOnly));
}

TEST(EditExportAdapterRun, DestructionJoinsActiveRenderAndRemovesStagingWithoutEventDelivery) {
    EnsureApplication();
    const QString fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    QTemporaryDir output;
    ASSERT_TRUE(output.isValid());
    QTemporaryDir config;
    ASSERT_TRUE(config.isValid());
    const auto previous_config = qgetenv("EXOSNAP_CONFIG_DIR");
    const auto restore_config = qScopeGuard([previous_config] { qputenv("EXOSNAP_CONFIG_DIR", previous_config); });
    qputenv("EXOSNAP_CONFIG_DIR", config.path().toUtf8());
    EditSessionAdapter session;
    QEventLoop imported;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &imported, &QEventLoop::quit);
    QTimer::singleShot(10000, &imported, &QEventLoop::quit);
    session.importMediaBatch({QUrl::fromLocalFile(QDir(fixtures).filePath(QStringLiteral("red.mkv"))),
                              QUrl::fromLocalFile(QDir(fixtures).filePath(QStringLiteral("blue.mkv")))},
                             true);
    imported.exec();
    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    ASSERT_EQ(session.workspace().assets().size(), 2u);
    for (const auto& asset : session.workspace().assets()) {
        engine::EditPlayerEngine player;
        std::string error;
        ASSERT_TRUE(player.Open(std::filesystem::path(asset.path), error)) << error;
        const auto metadata = player.AudioMetadata();
        engine::MonitorSelectionSnapshot selection{
            metadata.selection_media_id, {1}, {}, true, metadata.unverified_asset_binding};
        for (const auto& track : metadata.actual_tracks)
            selection.tracks.push_back({track.track_id, 1.0f, false});
        session.setAssetMonitorSelection(asset.id, std::move(selection));
    }
    ASSERT_TRUE(session.applyCrossfade(session.workspace().clips().front().id));
    // Enough real frames to observe an in-flight render even on a fast encoder.
    for (int i = 0; i < 100; ++i)
        session.appendAsset(session.workspace().assets().back().id);

    auto exporter = std::make_unique<EditExportAdapter>();
    exporter->setSession(&session);
    exporter->setOutputDirectoryProvider([&] { return output.path(); });
    exporter->startExport();
    QEventLoop review;
    QObject::connect(exporter.get(), &EditExportAdapter::stateChanged, &review, [&] {
        if (exporter->state() == EditExportAdapter::Review || exporter->state() == EditExportAdapter::Failed)
            review.quit();
    });
    QTimer::singleShot(10000, &review, &QEventLoop::quit);
    if (exporter->state() == EditExportAdapter::Preparing)
        review.exec();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Review) << exporter->errorText().toStdString();
    exporter->confirmPreparedExport();
    QPointer<QThread> worker = EditExportAdapterTestPeer::worker(*exporter);
    ASSERT_NE(worker, nullptr);
    std::atomic<bool> finished{false};
    std::atomic<bool> completion_timed_out{false};
    QSemaphore completion_gate;
    QObject observer;
    QObject::connect(
        worker, &QThread::finished, &observer,
        [&] {
            finished.store(true);
            completion_timed_out.store(!completion_gate.tryAcquire(1, 10000));
        },
        Qt::DirectConnection);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (EditExportAdapterTestPeer::progress(*exporter) < 1 && !finished.load() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool active = !finished.load() && EditExportAdapterTestPeer::progress(*exporter) >= 1;
    EXPECT_TRUE(active) << "A real render must produce frames before cancellation.";
    const auto staging = exporter->outputPath() + QStringLiteral(".tmp");
    EXPECT_TRUE(QFileInfo::exists(staging));
    exporter->cancel();
    EXPECT_EQ(exporter->state(), EditExportAdapter::Cancelling);
    EXPECT_FALSE(worker->wait(0)) << "UI cancellation must return before the gated worker exits.";
    completion_gate.release();
    exporter.reset();
    EXPECT_FALSE(completion_timed_out.load());
    EXPECT_TRUE(finished.load()) << "Destruction must join without processing queued completion.";
    EXPECT_TRUE(worker.isNull()) << "The finished QThread must be destroyed synchronously.";
    EXPECT_FALSE(QFileInfo::exists(staging));
    EXPECT_TRUE(QDir(output.path()).entryList(QDir::Files).isEmpty());
    // Keep teardown safe even when the ownership assertions fail.
    if (worker) {
        worker->wait();
        delete worker.data();
    }
    QCoreApplication::processEvents();
}

TEST(EditExportAdapterRun, DestructionReclaimsFinishedWorkerWithCompletionStillQueued) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QTemporaryDir source_directory;
    ASSERT_TRUE(source_directory.isValid());
    EditSessionAdapter session;
    const QString fixture_template =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString fixture = QDir(source_directory.path()).filePath(QStringLiteral("source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture_template, fixture));
    QEventLoop imported;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &imported, &QEventLoop::quit);
    QTimer::singleShot(10000, &imported, &QEventLoop::quit);
    session.importMedia(QUrl::fromLocalFile(fixture), true);
    imported.exec();
    ASSERT_EQ(session.workspace().clips().size(), 1u);
    auto exporter = std::make_unique<EditExportAdapter>();
    exporter->setSession(&session);
    exporter->setOutputDirectoryProvider([&] { return directory.path(); });
    exporter->startExport();
    QEventLoop review;
    QObject::connect(exporter.get(), &EditExportAdapter::stateChanged, &review, [&] {
        if (exporter->state() == EditExportAdapter::Review)
            review.quit();
    });
    QTimer::singleShot(10000, &review, &QEventLoop::quit);
    if (exporter->state() != EditExportAdapter::Review)
        review.exec();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Review) << exporter->errorText().toStdString();
    EXPECT_EQ(exporter->reviewOutcome().value(QStringLiteral("path")).toString(), QStringLiteral("Copy"));
    EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    const auto first_destination = exporter->outputPath();
    EXPECT_FALSE(QFileInfo::exists(first_destination + QStringLiteral(".tmp")));
    session.trimClip(session.workspace().clips().front().id, 0, 2000);
    EXPECT_EQ(exporter->state(), EditExportAdapter::Options);
    // A trimmed finished result is rendered exactly, never copied from a moved
    // keyframe. This fixture cannot be rendered, so only the explicit
    // original-stream intent can copy the cut.
    exporter->startExport();
    QEventLoop blocked;
    QObject::connect(exporter.get(), &EditExportAdapter::stateChanged, &blocked, [&] {
        if (exporter->state() == EditExportAdapter::Failed)
            blocked.quit();
    });
    QTimer::singleShot(10000, &blocked, &QEventLoop::quit);
    if (exporter->state() != EditExportAdapter::Failed)
        blocked.exec();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Failed);
    EXPECT_FALSE(exporter->errorText().isEmpty());
    exporter->setModeKey(QStringLiteral("originalStreams"));
    exporter->startExport();
    QTimer::singleShot(10000, &review, &QEventLoop::quit);
    if (exporter->state() != EditExportAdapter::Review)
        review.exec();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Review) << exporter->errorText().toStdString();
    EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    engine::RecorderConfig changed_config;
    changed_config.video_codec = engine::VideoCodec::H264;
    exporter->setRenderConfigProvider([&] { return changed_config; });
    exporter->confirmPreparedExport();
    EXPECT_EQ(exporter->state(), EditExportAdapter::Preparing);
    QTimer::singleShot(10000, &review, &QEventLoop::quit);
    review.exec();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Review) << exporter->errorText().toStdString();
    EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    engine::AudioMetadataReadResult metadata;
    {
        engine::EditPlayerEngine player;
        std::string error;
        ASSERT_TRUE(player.Open(std::filesystem::path(fixture.toStdWString()), error)) << error;
        metadata = player.AudioMetadata();
    }
    session.setAssetMonitorSelection(session.workspace().assets().front().id,
                                     {metadata.selection_media_id, {1}, {}, true, metadata.unverified_asset_binding});
    EXPECT_EQ(exporter->state(), EditExportAdapter::Options);
    exporter->startExport();
    QTimer::singleShot(10000, &review, &QEventLoop::quit);
    review.exec();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Review) << exporter->errorText().toStdString();
    EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    QFile source_file(fixture);
    const auto original_size = source_file.size();
    ASSERT_TRUE(source_file.open(QIODevice::Append));
    ASSERT_EQ(source_file.write("x", 1), 1);
    source_file.close();
    exporter->confirmPreparedExport();
    QEventLoop changed_source;
    QObject::connect(exporter.get(), &EditExportAdapter::stateChanged, &changed_source, [&] {
        if (exporter->state() == EditExportAdapter::Failed)
            changed_source.quit();
    });
    QTimer::singleShot(10000, &changed_source, &QEventLoop::quit);
    if (exporter->state() != EditExportAdapter::Failed)
        changed_source.exec();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Failed) << exporter->errorText().toStdString();
    EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    ASSERT_TRUE(source_file.open(QIODevice::ReadWrite));
    ASSERT_TRUE(source_file.resize(original_size));
    source_file.close();
    session.setAssetMonitorSelection(session.workspace().assets().front().id,
                                     {metadata.selection_media_id, {2}, {}, true, metadata.unverified_asset_binding});
    exporter->retry();
    QTimer::singleShot(10000, &review, &QEventLoop::quit);
    review.exec();
    ASSERT_EQ(exporter->state(), EditExportAdapter::Review) << exporter->errorText().toStdString();
    exporter->confirmPreparedExport();
    QPointer<QThread> worker = EditExportAdapterTestPeer::worker(*exporter);
    ASSERT_NE(worker, nullptr);
    ASSERT_TRUE(worker->wait(10000));
    EXPECT_TRUE(exporter->running());
    const auto destination = exporter->outputPath();
    exporter.reset();
    EXPECT_TRUE(worker.isNull());
    EXPECT_TRUE(QFileInfo::exists(destination));
    EXPECT_FALSE(QFileInfo::exists(destination + QStringLiteral(".tmp")));
    if (worker)
        delete worker.data();
    QCoreApplication::processEvents();
}

capability::CapabilitySet RenderEncoderCaps() {
    auto caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.runtime.adapter.vendor_id = 0x10DE;
    caps.runtime.nvidia.nvenc_codec_probed = true;
    caps.runtime.nvidia.nvenc_h264 = true;
    caps.runtime.nvidia.nvenc_av1 = true;
    caps.runtime.nvidia.nvenc_adv_h264 = {3, 2, true, true};
    caps.runtime.nvidia.nvenc_adv_av1 = {7, 2, true, true};
    capability::ApplyNvencCodecSupport(caps, caps.runtime.nvidia);
    capability::ApplyNvencAdvancedEncodeSupport(caps, caps.runtime.nvidia);
    return caps;
}

TEST(EditRenderConfig, PreservesExplicitPresetsAndSupportedAdvancedTuning) {
    OutputSettingsModel output;
    VideoSettingsModel video;
    video.rate_control = engine::RateControlMode::VariableBitrate;
    output.nvenc_tuning.bframes = 3;
    output.nvenc_tuning.b_ref_mode = engine::NvencBRefMode::Middle;
    output.nvenc_tuning.lookahead = true;
    output.nvenc_tuning.lookahead_depth = 8;
    output.nvenc_tuning.spatial_aq = true;
    output.nvenc_tuning.temporal_aq = true;
    output.nvenc_tuning.multipass = engine::NvencMultipass::QuarterResolution;
    engine::ResolvedEncoderDevice device;
    device.valid = true;
    device.adapter_luid = 42;
    device.vendor_id = 0x10DE;
    device.backend = engine::EncoderBackend::Nvenc;
    for (const auto preset : {engine::NvencPreset::P1, engine::NvencPreset::P7}) {
        output.nvenc_preset = preset;
        const auto config = BuildEditRenderConfig(output, video, RenderEncoderCaps(), device);
        const auto* tuning = engine::GetNvencTuning(config.backend_tuning);
        ASSERT_NE(tuning, nullptr);
        auto expected = output.nvenc_tuning;
        expected.preset = preset;
        EXPECT_EQ(*tuning, expected);
        EXPECT_TRUE(config.resolved_encoder_device.valid);
        EXPECT_EQ(config.resolved_encoder_device.adapter_luid, 42);
    }
}

TEST(EditRenderConfig, ResolvesAutomaticLookaheadForCodecAndRateControl) {
    OutputSettingsModel output;
    VideoSettingsModel video;
    output.video_codec = capability::VideoCodec::Av1;
    output.nvenc_lookahead_policy = capability::NvencLookaheadPolicy::Auto;
    output.nvenc_tuning.lookahead_depth = 5;
    video.rate_control = engine::RateControlMode::VariableBitrate;
    auto config = BuildEditRenderConfig(output, video, RenderEncoderCaps(), {});
    ASSERT_NE(engine::GetNvencTuning(config.backend_tuning), nullptr);
    EXPECT_TRUE(engine::GetNvencTuning(config.backend_tuning)->lookahead);
    EXPECT_EQ(engine::GetNvencTuning(config.backend_tuning)->lookahead_depth, 16u);
    video.rate_control = engine::RateControlMode::ConstantQuality;
    config = BuildEditRenderConfig(output, video, RenderEncoderCaps(), {});
    ASSERT_NE(engine::GetNvencTuning(config.backend_tuning), nullptr);
    EXPECT_FALSE(engine::GetNvencTuning(config.backend_tuning)->lookahead);
    EXPECT_FALSE(output.nvenc_tuning.lookahead);
    EXPECT_EQ(output.nvenc_tuning.lookahead_depth, 5u);
}

TEST(EditRenderConfig, ReconcilesSelectedAdapterLimitsWithoutChangingPreferences) {
    OutputSettingsModel output;
    VideoSettingsModel video;
    output.nvenc_preset = engine::NvencPreset::P7;
    output.nvenc_tuning.bframes = 20;
    output.nvenc_tuning.lookahead = true;
    output.nvenc_tuning.lookahead_depth = 32;
    auto config = BuildEditRenderConfig(output, video, RenderEncoderCaps(), {});
    ASSERT_NE(engine::GetNvencTuning(config.backend_tuning), nullptr);
    EXPECT_EQ(engine::GetNvencTuning(config.backend_tuning)->bframes, 3u);
    EXPECT_EQ(engine::GetNvencTuning(config.backend_tuning)->lookahead_depth, 16u);
    config = BuildEditRenderConfig(output, video, {}, {});
    ASSERT_NE(engine::GetNvencTuning(config.backend_tuning), nullptr);
    EXPECT_EQ(engine::GetNvencTuning(config.backend_tuning)->preset, engine::NvencPreset::P7);
    EXPECT_EQ(engine::GetNvencTuning(config.backend_tuning)->bframes, 0u);
    EXPECT_FALSE(engine::GetNvencTuning(config.backend_tuning)->lookahead);
    EXPECT_EQ(output.nvenc_tuning.bframes, 20u);
    EXPECT_EQ(output.nvenc_tuning.lookahead_depth, 32u);
}

TEST(EditRenderConfig, AutomaticDeviceUsesActualRenderAdapterAndItsTuningCapabilities) {
    std::vector<capability::AdapterInfo> adapters(2);
    for (size_t i = 0; i < adapters.size(); ++i) {
        adapters[i].vendor = capability::AdapterVendor::Nvidia;
        adapters[i].vendor_id = 0x10DE;
        adapters[i].device_id = static_cast<uint32_t>(i + 1);
        adapters[i].luid = static_cast<int64_t>(i + 10);
    }
    std::vector<capability::AdapterEncoderCapability> adapter_caps(2);
    adapter_caps[0].probed = true;
    adapter_caps[1].probed = true;
    adapter_caps[1].h264 = true;
    adapter_caps[1].max_bframes_h264 = 3;
    adapter_caps[1].lookahead_h264 = true;
    OutputSettingsModel output;
    output.nvenc_preset = engine::NvencPreset::P7;
    output.nvenc_tuning.bframes = 3;
    output.nvenc_tuning.lookahead = true;
    output.nvenc_tuning.lookahead_depth = 8;
    output.bit_depth = capability::BitDepth::Bit10;
    output.chroma_subsampling = capability::ChromaSubsampling::Cs444;
    VideoSettingsModel video;
    const auto device = ResolveEditEncoderDevice(adapters, adapter_caps, output.video_codec, video.encoder_device);
    ASSERT_TRUE(device.valid) << device.reason;
    EXPECT_EQ(device.adapter_luid, adapters[1].luid);
    const auto caps = capability::CapabilitySetForAdapter(RenderEncoderCaps(), adapters[1], adapter_caps[1]);
    const auto config = BuildEditRenderConfig(output, video, caps, device);
    const auto* tuning = engine::GetNvencTuning(config.backend_tuning);
    ASSERT_NE(tuning, nullptr);
    EXPECT_EQ(tuning->preset, engine::NvencPreset::P7);
    EXPECT_EQ(tuning->bframes, 3u);
    EXPECT_TRUE(tuning->lookahead);
    EXPECT_EQ(tuning->lookahead_depth, 8u);
    EXPECT_EQ(config.resolved_encoder_device.adapter_luid, adapters[1].luid);
    EXPECT_EQ(config.bit_depth, engine::BitDepth::Bit8);
    EXPECT_EQ(config.chroma, engine::ChromaSubsampling::Cs420);
    adapter_caps[0].h264 = true;
    EXPECT_EQ(ResolveEditEncoderDevice(adapters, adapter_caps, output.video_codec, video.encoder_device).adapter_luid,
              adapters[0].luid);
}

TEST(EditRenderConfig, DeviceResolutionRejectsUnprobedAndAmbiguousSelections) {
    capability::AdapterInfo adapter;
    adapter.vendor = capability::AdapterVendor::Nvidia;
    adapter.vendor_id = 0x10DE;
    adapter.luid = 42;
    std::vector<capability::AdapterInfo> adapters{adapter};
    std::vector<capability::AdapterEncoderCapability> adapter_caps(1);
    auto device = ResolveEditEncoderDevice(adapters, adapter_caps, capability::VideoCodec::H264, {});
    EXPECT_FALSE(device.valid);
    EXPECT_FALSE(device.reason.empty());
    engine::EncoderDevicePreference preference;
    preference.mode = engine::EncoderDevicePreference::Mode::Explicit;
    preference.device = capability::FingerprintFromAdapter(adapter);
    adapter_caps[0].probed = true;
    adapter_caps[0].h264 = true;
    device = ResolveEditEncoderDevice(adapters, adapter_caps, capability::VideoCodec::H264, preference);
    EXPECT_TRUE(device.valid) << device.reason;
    EXPECT_EQ(device.adapter_luid, 42);
    adapters.push_back(adapter);
    adapters.back().luid = 43;
    adapter_caps.push_back(adapter_caps.front());
    device = ResolveEditEncoderDevice(adapters, adapter_caps, capability::VideoCodec::H264, preference);
    EXPECT_FALSE(device.valid);
    EXPECT_NE(device.reason.find("Multiple adapters"), std::string::npos);
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

TEST(EditSessionDrag, PreviewIsFrozenAndCommitCreatesOneUndoStep) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    session.snapping = false;
    const auto id = session.workspace().clips().front().id;
    const auto revision = session.workspace().revision();
    session.beginClipDrag(id);
    session.updateClipDrag(1000);
    ASSERT_TRUE(session.dragValid());
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(session.workspace().clip(id)->start, 0);
    EXPECT_EQ(session.dragClips().size(), 2);
    session.updateClipDrag(2000);
    EXPECT_EQ(session.workspace().clip(id)->start, 0);
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().revision(), revision + 1);
    EXPECT_EQ(session.workspace().clip(id)->start, 2'000'000);
    session.undo();
    EXPECT_EQ(session.workspace().clip(id)->start, 0);
}

TEST(EditSessionDrag, CancellationInvalidAndStaleCandidatesHaveNoHistoryEntry) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    session.snapping = false;
    const auto id = session.workspace().clips().front().id;
    const auto revision = session.workspace().revision();
    session.beginClipDrag(id);
    session.updateClipDrag(-1000);
    EXPECT_FALSE(session.dragValid());
    EXPECT_FALSE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().revision(), revision);
    session.beginClipDrag(id);
    session.updateClipDrag(1000);
    session.cancelClipDrag();
    EXPECT_EQ(session.workspace().revision(), revision);
    session.beginClipDrag(id);
    session.updateClipDrag(1000);
    session.moveClip(id, 500, false);
    const auto changed_revision = session.workspace().revision();
    EXPECT_FALSE(session.dragActive());
    EXPECT_TRUE(session.dragClips().empty());
    EXPECT_FALSE(session.dragValid());
    EXPECT_FALSE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().revision(), changed_revision);
    EXPECT_FALSE(session.workspaceError().isEmpty());
}

TEST(EditSessionDrag, IndependentControlsAndFixedTrimPreserveUnselectedAudio) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    EXPECT_TRUE(session.snapping);
    EXPECT_FALSE(session.ripple);
    EXPECT_TRUE(session.autoCrossfade);
    EXPECT_TRUE(session.linkedSelection);
    session.linkedSelection = false;
    session.snapping = false;
    const auto id = session.workspace().clips().front().id;
    const auto other = session.workspace().clips().back().id;
    session.beginClipDrag(id, 0);
    session.updateClipDrag(1000);
    ASSERT_TRUE(session.dragValid());
    EXPECT_EQ(session.dragClips().size(), 1);
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().clip(id)->start, 1'000'000);
    EXPECT_EQ(session.workspace().clip(id)->source_in, 1'000'000);
    EXPECT_EQ(session.workspace().clip(other)->start, 0);
    EXPECT_EQ(session.workspace().clip(other)->source_in, 0);
    session.deleteSelected(false);
    ASSERT_EQ(session.workspace().clips().size(), 1);
    EXPECT_EQ(session.workspace().clips().front().id, other);
}

TEST(EditSessionDrag, ExtremeMillisecondsRejectWithoutMutatingHistory) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    const auto id = session.workspace().clips().front().id;
    const auto revision = session.workspace().revision();
    session.beginClipDrag(id);
    session.updateClipDrag((std::numeric_limits<qint64>::min)());
    EXPECT_FALSE(session.dragValid());
    EXPECT_FALSE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().revision(), revision);
    session.beginClipDrag(id);
    session.updateClipDrag((std::numeric_limits<qint64>::max)());
    EXPECT_FALSE(session.dragValid());
    session.cancelClipDrag();
    session.setClipFade(id, true, (std::numeric_limits<qint64>::max)());
    session.setFixedTimelineEnd(true, (std::numeric_limits<qint64>::max)());
    EXPECT_EQ(session.workspace().revision(), revision);
}

TEST(EditSessionBlankTime, CopyPasteSpacerAndPreviewGapClosureAreUndoable) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto id = session.workspace().clips().front().id;
    const auto track = session.workspace().clips().front().track;
    session.moveClip(id, 10000, false);
    session.requestSeek(0);
    session.selectGap(track, 5000);
    ASSERT_EQ(session.selectedGap().value(QStringLiteral("endMs")).toLongLong(), 10000);
    const auto gap_revision = session.workspace().revision();
    session.deleteSelected(false);
    EXPECT_EQ(session.workspace().revision(), gap_revision);
    session.copyBlankDuration();
    session.pasteSpacer();
    ASSERT_EQ(session.spacers().size(), 1);
    const auto spacer_id = session.spacers().front().toMap().value(QStringLiteral("id")).toULongLong();
    session.beginSpacerDrag(spacer_id, 1);
    session.updateClipDrag(-1000);
    ASSERT_TRUE(session.dragValid());
    EXPECT_EQ(session.spacers().front().toMap().value(QStringLiteral("durationMs")).toLongLong(), 10000);
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.spacers().front().toMap().value(QStringLiteral("durationMs")).toLongLong(), 9000);
    session.selectSpacer(spacer_id);
    session.deleteSelected(false);
    EXPECT_TRUE(session.spacers().isEmpty());
    EXPECT_EQ(session.workspace().clip(id)->start, 10'000'000);
    session.selectGap(track, 5000);
    session.previewCloseGap();
    ASSERT_TRUE(session.dragValid());
    EXPECT_EQ(session.workspace().clip(id)->start, 10'000'000);
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().clip(id)->start, 0);
    session.undo();
    EXPECT_EQ(session.workspace().clip(id)->start, 10'000'000);
}

TEST(EditSessionBlankTime, FixedEndAndTrackDeletionRejectStaleConfirmation) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.setFixedTimelineEnd(true, 20000);
    EXPECT_TRUE(session.fixedTimelineEnd());
    EXPECT_EQ(session.durationMs(), 20000);
    const auto id = session.workspace().clips().front().id;
    const auto track = session.workspace().clips().front().track;
    EXPECT_FALSE(session.deleteTrack(track));
    session.moveClip(id, 1000, false);
    const auto revision = session.workspace().revision();
    EXPECT_FALSE(session.deleteTrack(track, true));
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(session.workspace().tracks().size(), 2);
    EXPECT_FALSE(session.deleteTrack(track));
    EXPECT_TRUE(session.deleteTrack(track, true));
    EXPECT_EQ(session.durationMs(), 20000);
    session.undo();
    EXPECT_EQ(session.workspace().tracks().size(), 2);
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

TEST(EditSessionPreciseTime, SeekPreservesMicrosecondsAndNotifiesBothContractsOnce) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    qint64 precise = -1;
    qint64 legacy = -1;
    int precise_count = 0;
    int legacy_count = 0;
    QObject::connect(&session, &EditSessionAdapter::seekRequestedUs, &session, [&](qint64 value) {
        precise = value;
        ++precise_count;
    });
    QObject::connect(&session, &EditSessionAdapter::seekRequested, &session, [&](qint64 value) {
        legacy = value;
        ++legacy_count;
    });
    session.requestSeekUs(33'367);
    EXPECT_EQ(session.positionUs(), 33'367);
    EXPECT_EQ(session.workspace().playhead(), 33'367);
    EXPECT_EQ(precise, 33'367);
    EXPECT_EQ(legacy, 33);
    EXPECT_EQ(precise_count, 1);
    EXPECT_EQ(legacy_count, 1);
}

TEST(EditSessionPreciseTime, PlaybackPositionDoesNotIssueASeekAndKeepsSubMillisecondChanges) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    int seeks = 0;
    int positions = 0;
    QObject::connect(&session, &EditSessionAdapter::seekRequestedUs, &session, [&](qint64) { ++seeks; });
    QObject::connect(&session, &EditSessionAdapter::positionChanged, &session, [&] { ++positions; });
    session.setPositionUs(33'366);
    session.setPositionUs(33'367);
    EXPECT_EQ(session.positionUs(), 33'367);
    EXPECT_EQ(session.workspace().playhead(), 33'367);
    EXPECT_EQ(session.positionMs(), 33);
    EXPECT_EQ(positions, 2);
    EXPECT_EQ(seeks, 0);
}

TEST(EditSessionPreciseTime, LegacySeekStillPublishesThePreciseContract) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    qint64 precise = -1;
    QObject::connect(&session, &EditSessionAdapter::seekRequestedUs, &session, [&](qint64 value) { precise = value; });
    session.requestSeek(42);
    EXPECT_EQ(session.positionUs(), 42'000);
    EXPECT_EQ(precise, 42'000);
    session.requestSeekUs(-1);
    EXPECT_EQ(session.positionUs(), 0);
    session.requestSeekUs((std::numeric_limits<qint64>::max)());
    EXPECT_EQ(session.positionUs(), session.workspace().duration());
}

} // namespace
} // namespace exosnap::quick
