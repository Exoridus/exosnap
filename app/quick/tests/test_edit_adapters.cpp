#include "EditExportAdapter.h"
#include "EditInteractionController.h"
#include "EditPlaybackSnapshot.h"
#include "EditRenderText.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"
#include "EditTimelineModels.h"
#include "EditTrackLayoutModel.h"
#include "EditTransportFixture.h"
#include "EditViewportAdapter.h"
#include "models/EditTransportBounds.h"

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
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalBlocker>
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
#include <cmath>
#include <filesystem>
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
    static void chooseOutput(EditExportAdapter& adapter, const std::filesystem::path& path, bool replace) {
        adapter.chosen_output_ = path;
        adapter.overwrite_confirmed_ = replace;
    }
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

class EditSessionAdapterTestPeer {
  public:
    static QThreadPool& metadataPool(EditSessionAdapter& adapter) {
        return adapter.metadata_pool_;
    }
    static bool setFade(EditSessionAdapter& adapter, edit::Id id, bool incoming, edit::EdgeFade fade) {
        return adapter.applyCommand(edit::SetFade{id, incoming ? edit::FadeEdge::In : edit::FadeEdge::Out, fade});
    }
    static void scheduleSourceVerification(EditSessionAdapter& adapter, std::function<void()> task) {
        adapter.scheduleSourceVerification(std::move(task));
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

TEST(EditTimelineKeys, ExactBindingsSeparateFrameStepSampleStepNudgeAndClipSelection) {
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_Right, Qt::NoModifier, false), QStringLiteral("frame-next"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_Right, Qt::AltModifier, false), QStringLiteral("sample-next"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_Period, Qt::NoModifier, false), QStringLiteral("nudge-next"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_Down, Qt::NoModifier, false), QStringLiteral("boundary-next"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_Down, Qt::AltModifier, false), QStringLiteral("clip-next"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_Comma, Qt::ControlModifier | Qt::ShiftModifier, false),
              QStringLiteral("extend-start"));
}

TEST(EditClipPointer, QmlBridgeUsesSharedFadeOwnershipGainAndAxisGeometry) {
    EnsureApplication();
    EditInteractionController controller;
    QVariantMap audio{{QStringLiteral("video"), false},
                      {QStringLiteral("gainDb"), 0.0},
                      {QStringLiteral("fadeInOwned"), true},
                      {QStringLiteral("fadeOutOwned"), false}};
    EXPECT_EQ(controller.clipPointerTarget(audio, 100, 64, 18, 2, 2), EditInteractionController::PointerTrimStart);
    EXPECT_EQ(controller.clipPointerTarget(audio, 100, 64, 18, 98, 2), EditInteractionController::PointerFadeOut);
    EXPECT_EQ(controller.clipPointerTarget(audio, 100, 64, 18, 50, 32), EditInteractionController::PointerGain);
    EXPECT_EQ(controller.clipPointerTarget(audio, 100, 64, 18, 50, 50), EditInteractionController::PointerContent);
    EXPECT_EQ(controller.clipPointerTarget(audio, 20, 64, 18, 2, 2), EditInteractionController::PointerHeader);
    EXPECT_NEAR(controller.clipGainLineY(audio, 64, 18), 31.8, 0.001);
    EXPECT_FALSE(controller.clipPointerThreshold(EditInteractionController::PointerFadeIn, 2, 20, 10));
    EXPECT_TRUE(controller.clipPointerThreshold(EditInteractionController::PointerFadeIn, 10, 0, 10));
    EXPECT_FALSE(controller.clipPointerThreshold(EditInteractionController::PointerGain, 20, 2, 10));
    EXPECT_TRUE(controller.clipPointerThreshold(EditInteractionController::PointerContent, 6, 8, 10));
}

TEST(EditClipPointer, ModifiersHaveOneCapturedPolicyAndConflictingStructuralChordsRefuse) {
    EnsureApplication();
    EditInteractionController controller;
    const auto header =
        controller.clipPointerPolicy(EditInteractionController::PointerHeader, Qt::ControlModifier, false);
    ASSERT_TRUE(header.value("valid").toBool());
    EXPECT_TRUE(header.value("toggle").toBool());
    EXPECT_TRUE(header.value("insert").toBool());
    const auto shift =
        controller.clipPointerPolicy(EditInteractionController::PointerContent, Qt::ShiftModifier, false);
    EXPECT_TRUE(shift.value("range").toBool());
    EXPECT_TRUE(shift.value("anchorAtPlayhead").toBool());
    EXPECT_FALSE(shift.value("seek").toBool());
    const auto scope =
        controller.clipPointerPolicy(EditInteractionController::PointerContent, Qt::ControlModifier, true);
    EXPECT_TRUE(scope.value("preserveBounds").toBool());
    EXPECT_TRUE(controller.clipPointerPolicy(EditInteractionController::PointerGain, Qt::ControlModifier, false)
                    .value("fine")
                    .toBool());
    EXPECT_FALSE(
        controller
            .clipPointerPolicy(EditInteractionController::PointerHeader, Qt::ControlModifier | Qt::AltModifier, false)
            .value("valid")
            .toBool());
    EXPECT_FALSE(controller.clipPointerPolicy(EditInteractionController::PointerTrimEnd, Qt::ShiftModifier, false)
                     .value("valid")
                     .toBool());
}

TEST(EditTimelineKeys, RangeClipboardAndViewBindingsAreCentralAndRejectUnlistedChords) {
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_L, Qt::ControlModifier, false), QStringLiteral("loop"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_V, Qt::NoModifier, false), QStringLiteral("tool-select"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_R, Qt::NoModifier, false), QStringLiteral("tool-range"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_V, Qt::ControlModifier, false), QStringLiteral("paste"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_X, Qt::AltModifier, false), QStringLiteral("clear-range"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_C, Qt::ControlModifier, false), QStringLiteral("copy"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_X, Qt::ControlModifier, false), QStringLiteral("cut"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_Z, Qt::ShiftModifier, false), QStringLiteral("fit-timeline"));
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_Backspace, Qt::NoModifier, false), QStringLiteral("previous-view"));
    EXPECT_TRUE(ResolveTimelineKey(Qt::Key_L, Qt::ControlModifier | Qt::AltModifier, false).isEmpty());
    EXPECT_TRUE(ResolveTimelineKey(Qt::Key_L, Qt::ControlModifier, true).isEmpty());
    EXPECT_EQ(ResolveTimelineKey(Qt::Key_Right, Qt::NoModifier, true), QStringLiteral("frame-next"));
}

TEST(EditTimelineWheel, PixelDeltasOwnBothAxesExactlyOnce) {
    const auto action = ResolveTimelineWheel(Qt::NoModifier, {12, 24}, {120, 120}, false);
    EXPECT_DOUBLE_EQ(action.value("pan").toDouble(), -12);
    EXPECT_DOUBLE_EQ(action.value("scroll").toDouble(), -24);
    EXPECT_DOUBLE_EQ(action.value("zoom").toDouble(), 0);
}

TEST(EditTimelineWheel, SingleModifierPrefersVerticalAndRejectsConflictingChords) {
    const auto shift = ResolveTimelineWheel(Qt::ShiftModifier, {}, {240, 120}, false);
    EXPECT_DOUBLE_EQ(shift.value("pan").toDouble(), -40);
    EXPECT_DOUBLE_EQ(shift.value("scroll").toDouble(), 0);
    const auto control = ResolveTimelineWheel(Qt::ControlModifier, {}, {240, 120}, false);
    EXPECT_DOUBLE_EQ(control.value("zoom").toDouble(), 1);
    const auto alt = ResolveTimelineWheel(Qt::AltModifier, {}, {120, 0}, false);
    EXPECT_DOUBLE_EQ(alt.value("height").toDouble(), 8);
    const auto conflict = ResolveTimelineWheel(Qt::ControlModifier | Qt::ShiftModifier, {}, {0, 120}, false);
    EXPECT_DOUBLE_EQ(conflict.value("zoom").toDouble(), 0);
    EXPECT_DOUBLE_EQ(conflict.value("pan").toDouble(), 0);
    EXPECT_DOUBLE_EQ(conflict.value("scroll").toDouble(), 0);
}

TEST(EditTimelineWheel, ReservedSpaceOwnsTheSingleWheelAction) {
    const auto action = ResolveTimelineWheel(Qt::NoModifier, {}, {240, 120}, true);
    EXPECT_DOUBLE_EQ(action.value("pan").toDouble(), -40);
    EXPECT_DOUBLE_EQ(action.value("scroll").toDouble(), 0);
    EXPECT_DOUBLE_EQ(action.value("zoom").toDouble(), 0);
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

TEST(EditWorkspaceMarkers, SidecarUsesMetadataWorkerAndRejectsAReplacedContext) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto sidecar = directory.filePath("source.markers.json");
    QFile file(sidecar);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_GT(file.write(R"({"version":1,"markers":[{"timeMs":900,"type":"general","label":"sidecar"}]})"), 0);
    file.close();
    enum class Action { KeepContext, ReplaceContext, CancelImport };
    for (const auto state : {EditExportAdapter::Review, EditExportAdapter::Done, EditExportAdapter::Failed}) {
        for (const auto action : {Action::KeepContext, Action::ReplaceContext, Action::CancelImport}) {
            const bool replace_context = action == Action::ReplaceContext;
            for (const bool has_fallback : {false, true}) {
                SCOPED_TRACE(static_cast<int>(action));
                SCOPED_TRACE(has_fallback);
                QSemaphore entered;
                auto release = std::make_shared<QSemaphore>();
                EditSessionAdapter session;
                EditExportAdapter exporter;
                exporter.setSession(&session);
                auto& pool = EditSessionAdapterTestPeer::metadataPool(session);
                pool.start([&entered, release] {
                    entered.release();
                    release->acquire();
                });
                auto unblock = qScopeGuard([release] { release->release(); });
                ASSERT_TRUE(entered.tryAcquire(1, 5000));
                auto context = MakeContext();
                if (has_fallback)
                    context.markers = {MakeMarker(100, "fallback")};
                context.marker_sidecar_path = sidecar;
                session.setEditContext(context);
                ASSERT_EQ(session.markers().size(), has_fallback ? 1u : 0u);
                if (has_fallback)
                    EXPECT_EQ(session.markers().front().time_ms, 100);
                if (replace_context) {
                    context.marker_sidecar_path.clear();
                    context.markers = {MakeMarker(200, "replacement")};
                    session.setEditContext(context);
                }
                if (action == Action::CancelImport)
                    session.cancelPendingImports();
                exporter.applyVisualState(state, 100, QStringLiteral("result.mkv"), QString());
                int unsaved_notifications = 0;
                QObject::connect(&session, &EditSessionAdapter::unsavedEditsChanged, &session,
                                 [&unsaved_notifications] { ++unsaved_notifications; });
                release->release();
                unblock.dismiss();
                ASSERT_TRUE(pool.waitForDone(5000));
                QCoreApplication::processEvents();
                ASSERT_EQ(session.markers().size(), 1);
                EXPECT_EQ(session.markers().front().time_ms, replace_context ? 200 : 900);
                EXPECT_EQ(session.markers().front().label, replace_context ? "replacement" : "sidecar");
                EXPECT_EQ(exporter.state(), state);
                EXPECT_EQ(unsaved_notifications, !has_fallback && !replace_context ? 1 : 0);
            }
        }
    }
}

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

TEST(EditWorkspacePrecision, FrameDragAndVisibleRowsRetainMicrosecondsThroughUndo) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.snapping = false;
    const auto original = session.workspace().clips();
    const auto selected = session.selectedClip();
    session.beginClipDrag(selected);
    session.updateClipDragUs(16'666);
    ASSERT_TRUE(session.dragValid());
    ASSERT_TRUE(session.commitClipDrag());
    ASSERT_EQ(session.workspace().clips().size(), original.size());
    EXPECT_EQ(session.workspace().clips().front().start, 16'666);
    EXPECT_EQ(session.durationUs(), 10'016'666);
    const auto rows = session.visibleClipsUs(16'666, 16'667);
    ASSERT_EQ(rows.size(), 2);
    for (const auto& value : rows) {
        const auto row = value.toMap();
        EXPECT_EQ(row.value(QStringLiteral("startUs")).toLongLong(), 16'666);
        EXPECT_EQ(row.value(QStringLiteral("durationUs")).toLongLong(), 10000000);
    }
    session.undo();
    EXPECT_EQ(session.workspace().clips(), original);
    session.redo();
    EXPECT_EQ(session.workspace().clips().front().start, 16'666);
}

TEST(EditSelectionAdapter, ClipAndRangeModesDistinguishSelectionFromExplicitRangeClear) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto selected = session.selectedClip();
    int range_commits = 0;
    int explicit_clears = 0;
    QObject::connect(&session, &EditSessionAdapter::editRangeCommitted, [&](qint64, qint64) { ++range_commits; });
    QObject::connect(&session, &EditSessionAdapter::editRangeCleared, [&] { ++explicit_clears; });
    session.selectClipWithModifiers(selected);
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::ClipSelection);
    EXPECT_EQ(session.selectedClipIds().size(), 2);
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(1'000'001, 2'000'001, tracks));
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::RangeSelection);
    EXPECT_EQ(session.selectedClip(), 0);
    EXPECT_TRUE(session.selectedClipIds().isEmpty());
    EXPECT_EQ(session.editRange().value("startUs").toLongLong(), 1'000'000);
    EXPECT_EQ(session.editRange().value("endUs").toLongLong(), 2'000'000);
    EXPECT_EQ(range_commits, 1);
    session.selectClip(selected);
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::ClipSelection);
    EXPECT_TRUE(session.editRange().isEmpty());
    EXPECT_EQ(explicit_clears, 0);
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, tracks));
    session.clearEditRange();
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::NoSelection);
    EXPECT_EQ(explicit_clears, 1);
}

TEST(EditSelectionAdapter, RangeSplitSelectsInsidePiecesAndKeepsOneUndoTransaction) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto before = session.workspace().clips();
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(2'000'000, 4'000'000, tracks));
    session.splitSelected();
    ASSERT_EQ(session.workspace().clips().size(), 6u);
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::ClipSelection);
    ASSERT_EQ(session.selectedClipIds().size(), 2);
    for (const auto& id : session.selectedClipIds()) {
        const auto* clip = session.workspace().clip(id.toULongLong());
        ASSERT_NE(clip, nullptr);
        EXPECT_EQ(clip->start, 2'000'000);
        EXPECT_EQ(clip->end(), 4'000'000);
    }
    EXPECT_TRUE(session.editRange().isEmpty());
    session.undo();
    EXPECT_EQ(session.workspace().clips(), before);
}

TEST(EditSelectionAdapter, LockedRangeLiftRefusesAtomicallyAndSuccessfulLiftKeepsTheRange) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto before = session.workspace().clips();
    const auto revision = session.workspace().revision();
    const auto audio = session.workspace().tracks().back().id;
    ASSERT_TRUE(session.setTrackLocked(audio, true));
    EXPECT_EQ(session.workspace().revision(), revision);
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, tracks));
    session.deleteSelected();
    EXPECT_EQ(session.workspace().clips(), before);
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::RangeSelection);
    EXPECT_FALSE(session.workspaceError().isEmpty());
    ASSERT_TRUE(session.setTrackLocked(audio, false));
    session.deleteSelected();
    EXPECT_EQ(session.workspace().clips().size(), 4u);
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::RangeSelection);
    EXPECT_EQ(session.durationUs(), 10'000'000);
}

TEST(EditRangeGesture, CandidateCrossesAnchorWithoutChangingCommittedSelectionOrHistory) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, tracks));
    const auto original = session.editRange();
    const auto revision = session.workspace().revision();
    const auto history = session.workspace().historyBytes();
    int commits = 0;
    int seeks = 0;
    QObject::connect(&session, &EditSessionAdapter::editRangeCommitted, [&](qint64, qint64) { ++commits; });
    QObject::connect(&session, &EditSessionAdapter::seekRequestedUs, [&](qint64) { ++seeks; });
    ASSERT_TRUE(session.beginRangeGestureUs(4'000'000, tracks));
    EXPECT_TRUE(session.rangeGestureActive());
    EXPECT_FALSE(session.rangeGestureValid());
    session.updateRangeGestureUs(3'000'001);
    ASSERT_TRUE(session.rangeGestureValid());
    EXPECT_EQ(session.rangeGestureCandidate().value("startUs").toLongLong(), 3'000'000);
    EXPECT_EQ(session.rangeGestureCandidate().value("endUs").toLongLong(), 4'000'000);
    EXPECT_EQ(session.rangeGestureCandidate().value("anchorUs").toLongLong(), 4'000'000);
    EXPECT_EQ(session.editRange(), original);
    session.cancelRangeGesture();
    EXPECT_FALSE(session.rangeGestureActive());
    EXPECT_EQ(session.editRange(), original);
    EXPECT_EQ(commits, 0);
    ASSERT_TRUE(session.beginRangeGestureUs(4'000'000, tracks));
    session.updateRangeGestureUs(6'000'001);
    ASSERT_TRUE(session.commitRangeGesture());
    EXPECT_FALSE(session.rangeGestureActive());
    EXPECT_EQ(session.editRange().value("endUs").toLongLong(), 6'000'000);
    EXPECT_EQ(commits, 1);
    EXPECT_EQ(seeks, 0);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(session.workspace().historyBytes(), history);
}

TEST(EditCrossfadeAtCutAdapter, AdjacentCutStagesCompleteCandidateUntilCommitOrCancel) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto outgoing = session.workspace().clips().front().id;
    session.selectClip(outgoing);
    session.setPositionUs(5'000'000);
    session.splitSelected();
    ASSERT_EQ(session.workspace().clips().size(), 4U);
    const auto before = session.workspace().snapshot();
    ASSERT_TRUE(session.canCrossfadeAtCut(outgoing));
    ASSERT_TRUE(session.previewCrossfadeAtCut(outgoing));
    EXPECT_TRUE(session.dragActive());
    EXPECT_TRUE(session.dragValid());
    EXPECT_EQ(session.workspace().clips(), before->clips);
    EXPECT_EQ(session.workspace().revision(), before->revision);
    session.cancelClipDrag();
    EXPECT_FALSE(session.dragActive());
    EXPECT_EQ(session.workspace().clips(), before->clips);
    ASSERT_TRUE(session.previewCrossfadeAtCut(outgoing));
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().transitions().size(), 2U);
    EXPECT_EQ(session.workspace().duration(), 10'000'000);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), before->clips);
    EXPECT_EQ(session.workspace().transitions(), before->junctions);
}

TEST(EditCrossfadeAtCutAdapter, DropStagesSameValidatedCandidateWithoutChangingWorkspace) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.setPositionUs(5'000'000);
    session.splitSelected();
    const auto before = session.workspace().snapshot();
    ASSERT_TRUE(session.dropCrossfadeUs(5'000'000, 0));
    EXPECT_TRUE(session.dragActive());
    EXPECT_TRUE(session.dragValid());
    EXPECT_EQ(session.workspace().clips(), before->clips);
    EXPECT_EQ(session.workspace().revision(), before->revision);
    EXPECT_EQ(session.workspace().duration(), 10'000'000);
    session.cancelClipDrag();
}

TEST(EditRemoveOverlapAdapter, StructuralCandidatePreservesCommittedGeometryAndOriginalSelectionUntilCommit) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto outgoing = session.workspace().clips().front().id;
    session.setPositionUs(5'000'000);
    session.splitSelected();
    ASSERT_TRUE(session.previewCrossfadeAtCut(outgoing));
    ASSERT_TRUE(session.commitClipDrag());
    const auto before = session.workspace().snapshot();
    const auto junction_id = before->junctions.front().id;
    QVariantList tracks;
    for (const auto& track : before->tracks)
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, tracks));
    const auto range = session.editRange();
    const auto position = session.positionUs();
    ASSERT_TRUE(session.previewRemoveOverlap(junction_id));
    EXPECT_TRUE(session.dragActive());
    EXPECT_TRUE(session.dragValid());
    EXPECT_EQ(session.workspace().clips(), before->clips);
    EXPECT_EQ(session.workspace().transitions(), before->junctions);
    EXPECT_EQ(session.workspace().revision(), before->revision);
    session.cancelClipDrag();
    EXPECT_EQ(session.editRange(), range);
    ASSERT_TRUE(session.previewRemoveOverlap(junction_id));
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_TRUE(session.workspace().transitions().empty());
    EXPECT_EQ(session.workspace().revision(), before->revision + 1);
    EXPECT_EQ(session.workspace().duration(), 10'000'000);
    EXPECT_EQ(session.positionUs(), position);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), before->clips);
    EXPECT_EQ(session.workspace().transitions(), before->junctions);
}

TEST(EditRemoveOverlapAdapter, ALinkedLockRefusesBeforePublishingOrChangingSelectionAndHistory) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto outgoing = session.workspace().clips().front().id;
    session.setPositionUs(5'000'000);
    session.splitSelected();
    ASSERT_TRUE(session.previewCrossfadeAtCut(outgoing));
    ASSERT_TRUE(session.commitClipDrag());
    const auto junction_id = session.workspace().transitions().front().id;
    session.selectClip(outgoing);
    ASSERT_TRUE(session.setTrackLocked(session.workspace().tracks().back().id, true));
    const auto before = session.workspace().snapshot();
    const auto selection = session.selectedClipIds();
    const auto history = session.workspace().historyBytes();
    EXPECT_FALSE(session.previewRemoveOverlap(junction_id));
    EXPECT_FALSE(session.dragActive());
    EXPECT_EQ(session.selectedClipIds(), selection);
    EXPECT_EQ(session.workspace().clips(), before->clips);
    EXPECT_EQ(session.workspace().revision(), before->revision);
    EXPECT_EQ(session.workspace().historyBytes(), history);
    EXPECT_FALSE(session.workspaceError().isEmpty());
}

TEST(EditRangeScope, DirectAuthoredCommandRefusesWhileScopeReviewKeepsItsCandidate) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, tracks));
    const auto audio_track = session.workspace().tracks().back().id;
    const auto audio = std::find_if(session.workspace().clips().begin(), session.workspace().clips().end(),
                                    [audio_track](const auto& clip) { return clip.track == audio_track; });
    ASSERT_NE(audio, session.workspace().clips().end());
    const auto audio_id = audio->id;
    ASSERT_TRUE(session.previewRangeScope({QVariant::fromValue<qulonglong>(audio_track)}));
    const auto committed = session.editRange();
    const auto candidate = session.rangeGestureCandidate();
    const auto snapshot = session.workspace().snapshot();
    EXPECT_FALSE(session.setClipGain(audio_id, -3.0));
    EXPECT_TRUE(session.rangeScopeReviewActive());
    EXPECT_EQ(session.rangeGestureCandidate(), candidate);
    EXPECT_EQ(session.editRange(), committed);
    EXPECT_EQ(session.workspace().clips(), snapshot->clips);
    EXPECT_EQ(session.workspace().revision(), snapshot->revision);
    session.cancelRangeGesture();
    EXPECT_TRUE(session.setClipGain(audio_id, -3.0));
}

TEST(EditRangeGesture, AuthoredEditCancelsCandidateWithoutPublishingItsRange) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.beginRangeGestureUs(1'000'000, tracks));
    session.updateRangeGestureUs(2'000'000);
    ASSERT_TRUE(session.rangeGestureValid());
    session.moveClipUs(session.selectedClip(), 1'000'000);
    EXPECT_FALSE(session.rangeGestureActive());
    EXPECT_FALSE(session.commitRangeGesture());
    EXPECT_TRUE(session.editRange().isEmpty());
}

TEST(EditTrackAuthored, GainMuteAndRenameAreBoundedUndoableAndRespectLocksWithoutSeeking) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto audio_track = session.workspace().tracks().back().id;
    const auto audio_clip = session.workspace().clips().back().id;
    const auto video_clip = session.workspace().clips().front().id;
    int seeks = 0;
    QObject::connect(&session, &EditSessionAdapter::seekRequestedUs, [&](qint64) { ++seeks; });
    ASSERT_TRUE(session.setClipGain(audio_clip, -12.3));
    EXPECT_DOUBLE_EQ(session.workspace().clip(audio_clip)->gain_db, -12.3);
    const auto revision = session.workspace().revision();
    EXPECT_FALSE(session.setClipGain(audio_clip, 24.1));
    EXPECT_FALSE(session.setClipGain(audio_clip, -60.1));
    EXPECT_FALSE(session.setClipGain(video_clip, 0));
    EXPECT_FALSE(session.setTrackGain(audio_track, 12.1));
    EXPECT_FALSE(session.setTrackGain(audio_track, -60.1));
    EXPECT_EQ(session.workspace().revision(), revision);
    ASSERT_TRUE(session.setTrackGain(audio_track, 12));
    ASSERT_TRUE(session.setTrackEnabled(audio_track, false));
    EXPECT_FALSE(session.workspace().tracks().back().edit_enabled);
    ASSERT_TRUE(session.setTrackName(audio_track, QStringLiteral("Commentary")));
    EXPECT_EQ(session.workspace().tracks().back().name, "Commentary");
    session.undo();
    EXPECT_NE(session.workspace().tracks().back().name, "Commentary");
    ASSERT_TRUE(session.setTrackLocked(audio_track, true));
    EXPECT_FALSE(session.setClipGain(audio_clip, 0));
    EXPECT_FALSE(session.setTrackGain(audio_track, 0));
    EXPECT_FALSE(session.setTrackEnabled(audio_track, true));
    EXPECT_FALSE(session.setTrackName(audio_track, QStringLiteral("Locked")));
    EXPECT_EQ(seeks, 0);
}

TEST(EditPointerSelection, ControlRemovalWaitsForReleaseAndCancelRestoresPressSelection) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto clip = session.workspace().clips().front().id;
    session.selectClip(clip);
    const auto selected = session.selectedClipIds();
    ASSERT_EQ(selected.size(), 2);
    ASSERT_TRUE(session.beginClipPointerSelection(clip, true));
    EXPECT_EQ(session.selectedClipIds(), selected);
    session.finishPointerSelection(false, false);
    EXPECT_TRUE(session.selectedClipIds().isEmpty());
    session.selectClip(clip);
    ASSERT_TRUE(session.beginClipPointerSelection(clip, true));
    session.finishPointerSelection(false, true);
    EXPECT_EQ(session.selectedClipIds(), selected);
    session.clearSelection();
    ASSERT_TRUE(session.beginClipPointerSelection(clip));
    EXPECT_EQ(session.selectedClipIds(), selected);
    session.finishPointerSelection(true);
    EXPECT_TRUE(session.selectedClipIds().isEmpty());
}

TEST(EditPointerSelection, ClipAndRangeCancellationRestoreExactPrePressRange) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto clip = session.workspace().clips().front().id;
    const auto track = session.workspace().clips().front().track;
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, {QVariant::fromValue<qulonglong>(track)}, 2'000'000));
    const auto original = session.editRange();
    const auto revision = session.workspace().revision();
    ASSERT_TRUE(session.beginClipPointerSelection(clip));
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::ClipSelection);
    session.beginClipDrag(clip);
    session.updateClipDragUs(1'000'000);
    session.cancelClipDrag();
    EXPECT_EQ(session.editRange(), original);
    ASSERT_TRUE(session.beginClipPointerSelection(clip));
    ASSERT_TRUE(session.beginRangeGestureUs(3'000'000, {QVariant::fromValue<qulonglong>(track)}));
    session.updateRangeGestureUs(4'000'000);
    session.cancelRangeGesture();
    EXPECT_EQ(session.editRange(), original);
    EXPECT_EQ(session.workspace().revision(), revision);
}

TEST(EditPointerSelection, ContentReplacesMultiSelectionAndEmptyLaneCancelKeepsPressSeek) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.appendAsset(session.workspace().assets().front().id);
    session.selectAllClips();
    const auto selected = session.selectedClipIds();
    ASSERT_EQ(selected.size(), 4);
    const auto clip = session.workspace().clips().front().id;
    ASSERT_TRUE(session.beginClipPointerSelection(clip, false, false, false));
    EXPECT_EQ(session.selectedClipIds().size(), 2);
    session.finishPointerSelection(true);
    EXPECT_EQ(session.selectedClipIds(), selected);
    ASSERT_TRUE(session.applyTimeValue(0, QStringLiteral("timelineEnd"), 30'000'000));
    const auto track = session.workspace().tracks().front().id;
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, {QVariant::fromValue<qulonglong>(track)}));
    const auto range = session.editRange();
    ASSERT_TRUE(session.beginLanePointerSelection(track, 25'000'000));
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::GapSelection);
    EXPECT_EQ(session.positionUs(), 25'000'000);
    ASSERT_TRUE(session.beginRangeGestureUs(25'000'000, {QVariant::fromValue<qulonglong>(track)}));
    session.updateRangeGestureUs(26'000'000);
    session.cancelRangeGesture();
    EXPECT_EQ(session.editRange(), range);
    EXPECT_EQ(session.positionUs(), 25'000'000);
}

TEST(EditJunctionPointer, ActualOverlapPairsOwnContentBeyondTheShorterTransitionWindow) {
    EnsureApplication();
    EditInteractionController controller;
    const QVariantList rows{QVariantMap{{"id", 70},
                                        {"outgoing", 11},
                                        {"incoming", 12},
                                        {"overlapStartUs", 4'000'000},
                                        {"overlapEndUs", 6'000'000},
                                        {"startUs", 4'800'000},
                                        {"endUs", 5'200'000}}};
    EXPECT_EQ(controller.junctionAt(rows, 11, 4'100'000), 11U);
    EXPECT_EQ(controller.junctionAt(rows, 12, 5'900'000), 11U);
    EXPECT_EQ(controller.junctionAt(rows, 12, 6'000'000), 0U);
    EXPECT_EQ(controller.junctionAt(rows, 13, 5'000'000), 0U);
    EXPECT_EQ(controller.clipPointerTarget({{"video", true}}, 100, 64, 18, 50, 30, true),
              EditInteractionController::PointerJunction);
    EXPECT_EQ(controller.clipPointerTarget({{"video", true}}, 100, 64, 18, 50, 8, true),
              EditInteractionController::PointerHeader);
    EXPECT_EQ(controller.clipPointerTarget({{"video", true}}, 5, 64, 18, 2, 30, true),
              EditInteractionController::PointerHeader);
    EXPECT_FALSE(controller.clipPointerThreshold(EditInteractionController::PointerJunction, 100, 100, 10));
}

TEST(EditJunctionPointer, OverlappingWindowHandlesChooseNearestTrueEndpointThenLastOrStart) {
    EnsureApplication();
    EditInteractionController controller;
    EXPECT_EQ(controller.junctionHandleAt(0.4, 0, 0.5), 1);
    EXPECT_EQ(controller.junctionHandleAt(0.1, 0, 0.5), 0);
    EXPECT_EQ(controller.junctionHandleAt(0.25, 0, 0.5), 0);
    EXPECT_EQ(controller.junctionHandleAt(0.25, 0, 0.5, 1), 1);
    EXPECT_EQ(controller.junctionHandleAt(20, 0, 0.5), -1);
    EXPECT_EQ(controller.junctionHandleAt(0, 1, 0), -1);
}

TEST(EditNarrowClipPointer, ActualDistanceStableIdsAndRepeatedEqualDistanceClicksChooseTargets) {
    EnsureApplication();
    EditInteractionController controller;
    EditTimelineClipModel model;
    model.setRows({QVariantMap{{"id", 8}, {"trackId", 3}, {"startUs", 0}, {"endUs", 2}},
                   QVariantMap{{"id", 2}, {"trackId", 3}, {"startUs", 0}, {"endUs", 2}},
                   QVariantMap{{"id", 1}, {"trackId", 4}, {"startUs", 0}, {"endUs", 2}},
                   QVariantMap{{"id", 4}, {"trackId", 3}, {"startUs", 10}, {"endUs", 11}}});
    EXPECT_EQ(controller.resolveNarrowClip(&model, 3, 1, 1), 2U);
    EXPECT_EQ(controller.resolveNarrowClip(&model, 3, 1, 1), 8U);
    EXPECT_EQ(controller.resolveNarrowClip(&model, 3, 1, 1), 2U);
    EXPECT_EQ(controller.resolveNarrowClip(&model, 3, 5, 1), 2U);
    EXPECT_EQ(controller.resolveNarrowClip(&model, 3, 7, 1), 4U);
    EXPECT_EQ(controller.resolveNarrowClip(&model, 3, 20, 1), 0U);
    EXPECT_EQ(controller.resolveNarrowClip(nullptr, 3, 1, 1), 0U);
    EXPECT_EQ(controller.resolveNarrowClip(&model, 3, 1, 0), 0U);
}

TEST(EditJunctionPointer, OverlayYieldsToSharedPairedClipTrimHeaderAndNarrowGeometry) {
    EnsureApplication();
    EditInteractionController controller;
    const QVariantMap row{{"outgoingStartUs", 0},
                          {"outgoingEndUs", 6'000'000},
                          {"incomingStartUs", 4'000'000},
                          {"incomingEndUs", 10'000'000},
                          {"video", true}};
    EXPECT_TRUE(controller.junctionPointerEligible(row, 0.0001, 5'000'000, 30, 64, 18));
    EXPECT_FALSE(controller.junctionPointerEligible(row, 0.0001, 4'010'000, 30, 64, 18));
    EXPECT_FALSE(controller.junctionPointerEligible(row, 0.0001, 5'990'000, 30, 64, 18));
    EXPECT_FALSE(controller.junctionPointerEligible(row, 0.0001, 5'000'000, 8, 64, 18));
    EXPECT_FALSE(controller.junctionPointerEligible(row, 0.0000001, 5'000'000, 30, 64, 18));
    EXPECT_FALSE(controller.junctionPointerEligible(row, 0.0001, 6'000'000, 30, 64, 18));
}

TEST(EditJunctionPointer, HardCutMarkerDragChangesOnlyItsCutAndLockedTrackRefuses) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto outgoing = session.workspace().clips().front().id;
    session.selectClip(outgoing);
    session.setPositionUs(5'000'000);
    session.splitSelected();
    ASSERT_TRUE(session.applyCrossfade(outgoing, 1000));
    const auto junction = session.workspace().transitions().front();
    ASSERT_TRUE(session.applyTimeValue(junction.id, QStringLiteral("junctionCut"), 4'500'000));
    const auto before = session.workspace().snapshot();
    session.beginJunctionDrag(outgoing, 2);
    session.updateClipDragUs(100'000);
    ASSERT_TRUE(session.dragValid());
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().clips(), before->clips);
    EXPECT_EQ(session.workspace().transitions().front().kind, edit::TransitionKind::HardCut);
    EXPECT_EQ(session.workspace().transitions().front().cut_us, 4'600'000);
    session.undo();
    EXPECT_EQ(session.workspace().transitions(), before->junctions);
    ASSERT_TRUE(session.setTrackLocked(junction.track, true));
    session.beginJunctionDrag(outgoing, 2);
    session.updateClipDragUs(100'000);
    EXPECT_FALSE(session.dragValid());
    EXPECT_FALSE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().transitions(), before->junctions);
}

TEST(EditJunctionPointer, SuccessfulMarkerCommitConsumesPressCaptureAndKeepsJunctionSelection) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto outgoing = session.workspace().clips().front().id;
    session.setPositionUs(5'000'000);
    session.splitSelected();
    ASSERT_TRUE(session.applyCrossfade(outgoing, 1000));
    const auto junction = session.workspace().transitions().front();
    ASSERT_TRUE(session.applyTimeValue(junction.id, QStringLiteral("junctionCut"), 4'500'000));
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, tracks));
    ASSERT_TRUE(session.beginClipPointerSelection(outgoing));
    session.selectTransition(outgoing);
    session.beginJunctionDrag(outgoing, 2);
    session.updateClipDragUs(100'000);
    ASSERT_TRUE(session.commitClipDrag());
    session.finishPointerSelection(false, true);
    EXPECT_EQ(session.selectedTransition(), outgoing);
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::JunctionSelection);
    EXPECT_TRUE(session.editRange().isEmpty());
}

TEST(EditJunctionPointer, MarkerPreviewPublishesOnlyChangedPairWithExactKindAndCut) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto outgoing = session.workspace().clips().front().id;
    session.setPositionUs(5'000'000);
    session.splitSelected();
    ASSERT_TRUE(session.applyCrossfade(outgoing, 1000));
    const auto junction = session.workspace().transitions().front();
    ASSERT_TRUE(session.applyTimeValue(junction.id, QStringLiteral("junctionCut"), 4'500'000));
    session.beginJunctionDrag(outgoing, 2);
    session.updateClipDragUs(100'000);
    const auto rows = session.dragTransitions();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.front().toMap().value("id").toULongLong(), junction.id);
    EXPECT_EQ(rows.front().toMap().value("cutUs").toLongLong(), 4'600'000);
    EXPECT_EQ(rows.front().toMap().value("kind").toString(), QStringLiteral("hardCut"));
    session.cancelClipDrag();
}

TEST(EditPointerNavigation, CapturedRowUsesMidpointHysteresisAndInvalidGeometryDoesNotChooseTrack) {
    EditInteractionController controller;
    QVariantList rows;
    for (int index = 0; index < 3; ++index)
        rows.push_back(QVariantMap{{"id", index + 101}, {"top", index * 100.0}, {"height", 100.0}});
    EXPECT_EQ(controller.dragTrackId(rows, 102, 250), 102);
    EXPECT_EQ(controller.dragTrackId(rows, 102, 254), 103);
    EXPECT_EQ(controller.dragTrackId(rows, 103, 146), 102);
    EXPECT_EQ(controller.dragTrackId(rows, 999, 254), 0);
    rows[1] = QVariantMap{{"id", 102}, {"top", 100}, {"height", -1}};
    EXPECT_EQ(controller.dragTrackId(rows, 102, 254), 0);
}

TEST(EditPlacementPreview, BoundsAndDurationIncludePastedClipsBeyondCommittedEndUntilCancel) {
    EnsureApplication();
    EditSessionAdapter session;
    engine::EditMediaMetadata metadata;
    metadata.duration_us = 10'000'000;
    metadata.fps_num = 60;
    metadata.fps_den = 1;
    metadata.has_audio = true;
    metadata.content_identity_valid = true;
    metadata.content_sha256.fill(27);
    engine::EditSourceStream video;
    video.kind = engine::TimelineStreamKind::Video;
    video.container_index = 0;
    video.fps_num = 60;
    video.fps_den = 1;
    engine::EditSourceStream audio;
    audio.kind = engine::TimelineStreamKind::Audio;
    audio.container_index = 1;
    audio.codec.channels = 2;
    metadata.streams = {video, audio};
    session.addProbedMediaForHarness(QStringLiteral("clipboard-source.mkv"), metadata, true);
    session.selectAllClips();
    ASSERT_TRUE(session.copyClips());
    ASSERT_TRUE(session.previewPasteClipsUs(20'000'000));
    EXPECT_EQ(session.durationUs(), 10'000'000);
    EXPECT_EQ(session.dragDurationUs(), 30'000'000);
    EXPECT_TRUE(session.dragBounds().value("valid").toBool());
    EXPECT_EQ(session.dragBounds().value("startUs").toLongLong(), 20'000'000);
    EXPECT_EQ(session.dragBounds().value("endUs").toLongLong(), 30'000'000);
    session.cancelClipDrag();
    EXPECT_EQ(session.dragDurationUs(), 10'000'000);
}

TEST(EditPointerSeek, ReleaseCanonicalizesContinuousRequestedTimeOnDefaultFrameGrid) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.requestSeekUs(12'345);
    EXPECT_EQ(session.positionUs(), 12'345);
    const auto revision = session.workspace().revision();
    session.finishPointerSeek();
    EXPECT_EQ(session.positionUs(), 16'666);
    EXPECT_EQ(session.workspace().revision(), revision);
}

TEST(EditPointerSeek, ExplicitAudioRangeUsesSampleGridOnRelease) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto audio = session.workspace().tracks().back().id;
    ASSERT_TRUE(session.setEditRangeUs(0, 1'000'000, {QVariant::fromValue<qulonglong>(audio)}));
    ASSERT_TRUE(session.timeValueUsesSamples(0, QStringLiteral("playhead")));
    session.requestSeekUs(12'345);
    session.finishPointerSeek();
    EXPECT_EQ(session.positionUs(), 12'354);
}

TEST(EditMagneticSnap, CapturedLoopBoundsWinEqualDistanceOverPlayhead) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.requestSeekUs(1'100'000);
    session.setLoopSnapBounds(1'000'000, 2'000'000);
    session.beginClipDrag(session.workspace().clips().front().id);
    session.updateClipDragUs(1'050'000, 0, -1, false, 60'000);
    ASSERT_TRUE(session.dragValid());
    EXPECT_EQ(session.dragClips().front().toMap().value("startUs").toLongLong(), 1'000'000);
}

TEST(EditMagneticSnap, SnapRemainsUntilTwoExtraPixelsThenReleasesToNearestCandidate) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.requestSeekUs(1'100'000);
    session.setLoopSnapBounds(1'000'000, 2'000'000);
    session.beginClipDrag(session.workspace().clips().front().id);
    session.updateClipDragUs(1'050'000, 0, -1, false, 60'000);
    session.updateClipDragUs(1'066'667, 0, -1, false, 60'000);
    EXPECT_EQ(session.dragClips().front().toMap().value("startUs").toLongLong(), 1'000'000);
    session.updateClipDragUs(1'083'333, 0, -1, false, 60'000);
    EXPECT_EQ(session.dragClips().front().toMap().value("startUs").toLongLong(), 1'100'000);
}

TEST(EditMagneticSnap, LoopReferenceIsCapturedAtPressBeforeGestureThreshold) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto clip = session.workspace().clips().front().id;
    session.setLoopSnapBounds(1'000'000, 2'000'000);
    ASSERT_TRUE(session.beginClipPointerSelection(clip));
    session.setLoopSnapBounds(3'000'000, 4'000'000);
    session.beginClipDrag(clip);
    session.updateClipDragUs(1'050'000, 0, -1, false, 60'000);
    EXPECT_EQ(session.dragClips().front().toMap().value("startUs").toLongLong(), 1'000'000);
}

TEST(EditFadeEnvelope, CurvesAreSampledThroughSharedEvaluatorWithExactEndpointDirection) {
    EditInteractionController controller;
    const auto points = controller.fadeEnvelopePoints(static_cast<int>(engine::CurveKind::Bias), 0.25);
    ASSERT_GE(points.size(), 3);
    EXPECT_DOUBLE_EQ(points.front().toDouble(), 0);
    EXPECT_DOUBLE_EQ(points.back().toDouble(), 1);
    const auto out = controller.fadeEnvelopePoints(static_cast<int>(engine::CurveKind::Bias), 0.25, true);
    ASSERT_EQ(out.size(), points.size());
    for (qsizetype index = 0; index < points.size(); ++index)
        EXPECT_DOUBLE_EQ(out[index].toDouble(), points[points.size() - index - 1].toDouble());
    EXPECT_TRUE(controller.fadeEnvelopePoints(999, 0.5).isEmpty());
}

TEST(EditPointerNavigation, EdgeScrollUsesBothAxesAndCapsAtSixHundredPixelsPerSecond) {
    EditInteractionController controller;
    EXPECT_EQ(controller.edgeAutoScroll({0, 100}, {500, 200}), QPointF(-600, 0));
    EXPECT_EQ(controller.edgeAutoScroll({488, 188}, {500, 200}), QPointF(300, 300));
    EXPECT_EQ(controller.edgeAutoScroll({-100, 300}, {500, 200}), QPointF(-600, 600));
    EXPECT_EQ(controller.edgeAutoScroll({10, 10}, {-1, 200}).x(), 0);
}

TEST(EditTrackAuthored, ReorderIsSameKindOnlyUndoableAndRejectsInvalidIndices) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.beginClipDrag(session.workspace().clips().front().id, -1, true);
    session.updateClipDragUs(0, 0, 0);
    ASSERT_TRUE(session.commitClipDrag());
    const auto tracks = session.workspace().tracks();
    ASSERT_EQ(tracks.size(), 3U);
    const auto video = tracks.front().id;
    ASSERT_TRUE(session.reorderTrack(video, 1));
    EXPECT_EQ(session.workspace().tracks()[1].id, video);
    EXPECT_FALSE(session.reorderTrack(video, 2));
    EXPECT_FALSE(session.reorderTrack(video, -1));
    EXPECT_FALSE(session.reorderTrack(video, 999));
    session.undo();
    EXPECT_EQ(session.workspace().tracks(), tracks);
}

TEST(EditRangeBand, BodyAndCrossedEdgeKeepCapturedAnchorUntilOneCommit) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(2'000'000, 4'000'000, tracks, 2'000'000));
    const auto revision = session.workspace().revision();
    ASSERT_TRUE(session.beginRangeBandGestureUs(3'000'000, 100'000));
    EXPECT_EQ(session.rangeBandTarget(), static_cast<int>(edit::RangePointerTarget::Body));
    session.updateRangeBandGestureUs(4'000'000);
    EXPECT_EQ(session.editRange().value("startUs").toLongLong(), 2'000'000);
    EXPECT_EQ(session.rangeGestureCandidate().value("startUs").toLongLong(), 3'000'000);
    EXPECT_EQ(session.rangeGestureCandidate().value("endUs").toLongLong(), 5'000'000);
    EXPECT_EQ(session.rangeGestureCandidate().value("anchorUs").toLongLong(), 3'000'000);
    ASSERT_TRUE(session.commitRangeGesture());
    ASSERT_TRUE(session.beginRangeBandGestureUs(4'000'000, 2'000'000));
    EXPECT_EQ(session.rangeBandTarget(), static_cast<int>(edit::RangePointerTarget::Start));
    session.updateRangeBandGestureUs(6'000'000);
    EXPECT_EQ(session.rangeGestureCandidate().value("startUs").toLongLong(), 5'000'000);
    EXPECT_EQ(session.rangeGestureCandidate().value("endUs").toLongLong(), 6'000'000);
    EXPECT_EQ(session.rangeGestureCandidate().value("anchorUs").toLongLong(), 5'000'000);
    ASSERT_TRUE(session.commitRangeGesture());
    ASSERT_TRUE(session.beginRangeBandGestureUs(6'000'000, 100'000));
    EXPECT_EQ(session.rangeBandTarget(), static_cast<int>(edit::RangePointerTarget::End));
    session.cancelRangeGesture();
    ASSERT_TRUE(session.beginRangeBandGestureUs(5'500'000, 600'000));
    EXPECT_EQ(session.rangeBandTarget(), static_cast<int>(edit::RangePointerTarget::End));
    session.cancelRangeGesture();
    EXPECT_EQ(session.workspace().revision(), revision);
}

TEST(EditRangeBand, EmptyBandCreatesAllTrackScopeAndInvalidReleaseKeepsOriginalRangeAndPlayhead) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.clearSelection();
    const auto revision = session.workspace().revision();
    int seeks = 0;
    QObject::connect(&session, &EditSessionAdapter::seekRequestedUs, [&](qint64) { ++seeks; });
    ASSERT_TRUE(session.beginRangeBandGestureUs(1'000'000, 100'000));
    EXPECT_EQ(session.rangeBandTarget(), static_cast<int>(edit::RangePointerTarget::Create));
    session.updateRangeBandGestureUs(2'000'000);
    ASSERT_TRUE(session.rangeGestureValid());
    EXPECT_EQ(session.rangeGestureCandidate().value("trackIds").toList().size(), session.workspace().tracks().size());
    ASSERT_TRUE(session.commitRangeGesture());
    const auto original = session.editRange();
    ASSERT_TRUE(session.beginRangeBandGestureUs(1'000'000, 100'000));
    session.updateRangeBandGestureUs(12'000'000);
    EXPECT_FALSE(session.rangeGestureValid());
    EXPECT_FALSE(session.commitRangeGesture());
    EXPECT_EQ(session.editRange(), original);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(seeks, 0);
}

TEST(EditRangeBand, CreatingOutsideAnAudioOnlyRangeUsesEveryCurrentTrack) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto audio = session.workspace().tracks().back().id;
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, {QVariant::fromValue<qulonglong>(audio)}));
    ASSERT_TRUE(session.beginRangeBandGestureUs(4'000'000, 100'000));
    session.updateRangeBandGestureUs(5'000'000);
    ASSERT_TRUE(session.rangeGestureValid());
    EXPECT_EQ(session.rangeGestureCandidate().value("trackIds").toList().size(), session.workspace().tracks().size());
}

void CheckGestureSourcePublication(bool mismatch, bool remove_overlap = false) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto fixture =
        QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const auto path = directory.filePath(QStringLiteral("held-source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, path));
    engine::AudioManifest1 manifest;
    manifest.media_id = engine::NewId<engine::MediaId>();
    manifest.recording_id = engine::NewId<engine::RecordingId>();
    manifest.segment_id = engine::NewId<engine::SegmentId>();
    ASSERT_TRUE(engine::WriteMediaIdentitySidecar(std::filesystem::path(path.toStdWString()), manifest).succeeded());
    if (mismatch) {
        QFile sidecar(path + QStringLiteral(".exosnap.json"));
        ASSERT_TRUE(sidecar.open(QIODevice::ReadOnly));
        auto json = QJsonDocument::fromJson(sidecar.readAll()).object();
        sidecar.close();
        ASSERT_TRUE(json.contains(QStringLiteral("sha256")));
        json[QStringLiteral("sha256")] = QString(64, QLatin1Char('0'));
        ASSERT_TRUE(sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(sidecar.write(QJsonDocument(json).toJson(QJsonDocument::Compact)), 0);
        sidecar.close();
    }
    EditSessionAdapter session;
    bool began = false;
    edit::Revision revision = 0;
    edit::Id asset = 0;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, [&] {
        if (began || session.workspace().clips().empty())
            return;
        began = true;
        const auto& clip = session.workspace().clips().front();
        asset = clip.asset;
        if (remove_overlap) {
            const auto outgoing = clip.id;
            const auto fps = session.workspace().snapshot()->output_fps;
            const auto cut = fps.nearestFrame(clip.duration() / 2);
            ASSERT_TRUE(cut);
            session.setPositionUs(*cut);
            session.splitSelected();
            ASSERT_TRUE(session.previewCrossfadeAtCut(outgoing, *fps.frameTime(4)));
            ASSERT_TRUE(session.commitClipDrag());
            ASSERT_FALSE(session.workspace().transitions().empty());
            ASSERT_TRUE(session.previewRemoveOverlap(session.workspace().transitions().front().id));
        } else {
            session.beginClipDrag(clip.id);
            session.updateClipDragUs(1'000'000);
        }
        revision = session.workspace().revision();
    });
    session.importMedia(QUrl::fromLocalFile(path), true);
    QElapsedTimer deadline;
    deadline.start();
    while ((!began || (!session.sourceProof(asset) && !session.sourceVerificationFailed(asset))) &&
           deadline.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    ASSERT_TRUE(began);
    ASSERT_EQ(session.workspace().revision(), revision);
    ASSERT_TRUE(session.dragActive());
    const auto before = session.workspace().clips();
    if (mismatch) {
        ASSERT_TRUE(session.sourceVerificationFailed(asset));
        EXPECT_FALSE(session.dragValid());
        EXPECT_FALSE(session.dragReason().isEmpty());
        EXPECT_FALSE(session.commitClipDrag());
        EXPECT_EQ(session.workspace().revision(), revision);
        EXPECT_EQ(session.workspace().clips(), before);
    } else {
        ASSERT_TRUE(session.sourceProof(asset));
        EXPECT_TRUE(session.dragValid());
        session.cancelClipDrag();
    }
}

TEST(EditGestureSource, SuccessfulProofAtSameAuthoredRevisionKeepsCapturedCandidate) {
    CheckGestureSourcePublication(false);
}

TEST(EditGestureSource, KnownExpectedHashFailureRefusesCapturedCandidateWithoutAuthoredMutation) {
    CheckGestureSourcePublication(true);
}

TEST(EditRemoveOverlapAdapter, PendingProofSuccessPreservesTheCapturedStructuralCandidate) {
    CheckGestureSourcePublication(false, true);
}

TEST(EditRemoveOverlapAdapter, PendingExpectedHashFailureRefusesTheCapturedStructuralCandidate) {
    CheckGestureSourcePublication(true, true);
}

TEST(EditFadePointer, FineMotionAccumulatesOneCanonicalSamplePerFourPixelsAtCapturedEndpoint) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto audio = session.workspace().clips().back().id;
    ASSERT_TRUE(session.applyTimeValue(audio, QStringLiteral("fadeIn"), 1'000'000));
    session.beginClipDrag(audio, 2);
    session.updateClipFadeDragPixels(3, 0.00001, true);
    EXPECT_TRUE(session.dragClips().isEmpty());
    session.updateClipFadeDragPixels(4, 0.00001, true);
    ASSERT_FALSE(session.dragClips().isEmpty());
    EXPECT_EQ(session.dragClips().front().toMap().value("fadeInUs").toLongLong(), 1'000'021);
    EXPECT_EQ(session.workspace().clip(audio)->fade_in.duration_us, 1'000'000);
    session.updateClipFadeDragPixels(8, 0.00001, true);
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().clip(audio)->fade_in.duration_us, 1'000'042);
}

TEST(EditTimelineActions, FrameAndSecondNavigationAndSelectionBoundsUseExactBackendState) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto clip = session.workspace().clips().front().id;
    session.selectClip(clip);
    auto bounds = session.selectionBounds();
    ASSERT_TRUE(bounds.value("valid").toBool());
    EXPECT_EQ(bounds.value("startUs").toLongLong(), 0);
    EXPECT_EQ(bounds.value("endUs").toLongLong(), 10'000'000);
    const auto revision = session.workspace().revision();
    int inspections = 0;
    QObject::connect(&session, &EditSessionAdapter::inspectionRequested, [&] { ++inspections; });
    ASSERT_TRUE(session.executeTimelineAction(QStringLiteral("frame-next")));
    EXPECT_EQ(session.positionUs(), session.workspace().snapshot()->output_fps.frameTime(1).value());
    EXPECT_EQ(inspections, 1);
    ASSERT_TRUE(session.executeTimelineAction(QStringLiteral("second-next")));
    EXPECT_EQ(session.positionUs(), 1'000'000 + session.workspace().snapshot()->output_fps.frameTime(1).value());
    EXPECT_EQ(inspections, 1);
    ASSERT_TRUE(session.executeTimelineAction(QStringLiteral("timeline-end")));
    EXPECT_EQ(session.positionUs(), 10'000'000);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_FALSE(session.executeTimelineAction(QStringLiteral("invented-action")));
}

TEST(EditTimelineActions, AudioSampleNavigationRequiresExplicitIndependentAudioScope) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.selectClip(session.workspace().clips().front().id);
    EXPECT_FALSE(session.executeTimelineAction(QStringLiteral("sample-next")));
    EXPECT_EQ(session.positionUs(), 0);
    const auto audio_track = session.workspace().tracks().back().id;
    ASSERT_TRUE(session.setEditRangeUs(0, 1'000'000, {QVariant::fromValue<qulonglong>(audio_track)}));
    ASSERT_TRUE(session.executeTimelineAction(QStringLiteral("sample-next")));
    EXPECT_EQ(session.positionUs(), 21);
    ASSERT_TRUE(session.executeTimelineAction(QStringLiteral("sample-next")));
    EXPECT_EQ(session.positionUs(), 42);
    ASSERT_TRUE(session.executeTimelineAction(QStringLiteral("sample-previous")));
    EXPECT_EQ(session.positionUs(), 21);
}

TEST(EditGainGesture, PreviewUsesCapturedRelativeGainAndCommitsOnlyOnRelease) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto audio = session.workspace().clips().back().id;
    const auto revision = session.workspace().revision();
    ASSERT_TRUE(session.beginClipGainDrag(audio));
    session.updateClipGainDrag(-10);
    ASSERT_TRUE(session.dragValid());
    EXPECT_DOUBLE_EQ(session.workspace().clip(audio)->gain_db, 0);
    EXPECT_EQ(session.workspace().revision(), revision);
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_DOUBLE_EQ(session.workspace().clip(audio)->gain_db, 2);
    session.undo();
    ASSERT_TRUE(session.beginClipGainDrag(audio, true));
    session.updateClipGainDrag(-12);
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_DOUBLE_EQ(session.workspace().clip(audio)->gain_db, 0.2);
    ASSERT_TRUE(session.beginClipGainDrag(audio));
    session.updateClipGainDrag(1000);
    session.cancelClipDrag();
    EXPECT_DOUBLE_EQ(session.workspace().clip(audio)->gain_db, 0.2);
}

TEST(EditTimeValue, ExplicitClipTargetsApplyEveryExactFieldAndRejectUnavailableTargets) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto clip = session.workspace().clips().front().id;
    session.clearSelection();
    ASSERT_TRUE(session.applyTimeValue(clip, QStringLiteral("clipStart"), 1'000'000));
    EXPECT_EQ(session.workspace().clip(clip)->start, 1'000'000);
    ASSERT_TRUE(session.applyTimeValue(clip, QStringLiteral("sourceIn"), 1'000'000));
    EXPECT_EQ(session.workspace().clip(clip)->source_in, 1'000'000);
    ASSERT_TRUE(session.applyTimeValue(clip, QStringLiteral("sourceOut"), 9'000'000));
    EXPECT_EQ(session.workspace().clip(clip)->source_out, 9'000'000);
    ASSERT_TRUE(session.applyTimeValue(clip, QStringLiteral("duration"), 5'000'000));
    EXPECT_EQ(session.workspace().clip(clip)->duration(), 5'000'000);
    ASSERT_TRUE(session.applyTimeValue(clip, QStringLiteral("fadeIn"), 500'000));
    ASSERT_TRUE(session.applyTimeValue(clip, QStringLiteral("fadeOut"), 500'000));
    EXPECT_EQ(session.workspace().clip(clip)->fade_in.duration_us, 500'000);
    EXPECT_EQ(session.workspace().clip(clip)->fade_out.duration_us, 500'000);
    const auto revision = session.workspace().revision();
    EXPECT_FALSE(session.applyTimeValue(clip, QStringLiteral("sourceIn"), 10'000'000));
    EXPECT_FALSE(session.applyTimeValue(999'999, QStringLiteral("clipStart"), 0));
    EXPECT_FALSE(session.applyTimeValue(clip, QStringLiteral("unknown"), 0));
    EXPECT_EQ(session.workspace().revision(), revision);
}

TEST(EditTimeValue, RangeAndJunctionFieldsUseCapturedIdsAndOneValidatedCommand) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto first = session.workspace().clips().front().id;
    session.setEditContext(MakeContext(10));
    ASSERT_TRUE(session.applyCrossfade(first, 1000));
    ASSERT_FALSE(session.workspace().transitions().empty());
    const auto junction = session.workspace().transitions().front().id;
    ASSERT_TRUE(session.applyTimeValue(junction, QStringLiteral("junctionStart"), 9'200'000));
    ASSERT_TRUE(session.applyTimeValue(junction, QStringLiteral("junctionEnd"), 9'800'000));
    ASSERT_TRUE(session.applyTimeValue(junction, QStringLiteral("junctionDuration"), 400'000));
    ASSERT_TRUE(session.applyTimeValue(junction, QStringLiteral("junctionCut"), 9'500'000));
    const auto found = std::find_if(session.workspace().transitions().begin(), session.workspace().transitions().end(),
                                    [junction](const auto& value) { return value.id == junction; });
    ASSERT_NE(found, session.workspace().transitions().end());
    EXPECT_EQ(found->kind, edit::TransitionKind::HardCut);
    EXPECT_EQ(found->cut_us, 9'500'000);
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(2'000'000, 4'000'000, tracks));
    ASSERT_TRUE(session.applyTimeValue(0, QStringLiteral("rangeStart"), 1'000'000));
    ASSERT_TRUE(session.applyTimeValue(0, QStringLiteral("rangeEnd"), 5'000'000));
    ASSERT_TRUE(session.applyTimeValue(0, QStringLiteral("rangeDuration"), 2'000'000));
    EXPECT_EQ(session.editRange().value("startUs").toLongLong(), 1'000'000);
    EXPECT_EQ(session.editRange().value("endUs").toLongLong(), 3'000'000);
    EXPECT_EQ(session.editRange().value("anchorUs").toLongLong(), 1'000'000);
    ASSERT_TRUE(session.applyTimeValue(0, QStringLiteral("timelineEnd"), 30'000'000));
    EXPECT_EQ(session.durationUs(), 30'000'000);
    session.undo();
    EXPECT_EQ(session.durationUs(), 19'000'000);
}

TEST(EditTimeContext, FieldGridIncludesLinkedTargetsAndExplicitRangeScope) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto video = session.workspace().clips().front().id;
    const auto audio = session.workspace().clips().back().id;
    EXPECT_FALSE(session.timeValueUsesSamples(audio, QStringLiteral("sourceIn")));
    EXPECT_TRUE(session.timeValueUsesSamples(audio, QStringLiteral("fadeIn")));
    EXPECT_FALSE(session.timeValueUsesSamples(video, QStringLiteral("fadeOut")));
    session.linkedSelection = false;
    EXPECT_TRUE(session.timeValueUsesSamples(audio, QStringLiteral("clipStart")));
    const auto audio_track = session.workspace().tracks().back().id;
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, {QVariant::fromValue<qulonglong>(audio_track)}));
    EXPECT_TRUE(session.timeValueUsesSamples(0, QStringLiteral("rangeStart")));
    EXPECT_EQ(session.editRange().value("durationUs").toLongLong(), 1'000'000);
    QVariantList all;
    for (const auto& track : session.workspace().tracks())
        all.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, all));
    EXPECT_FALSE(session.timeValueUsesSamples(0, QStringLiteral("rangeEnd")));
    EXPECT_FALSE(session.timeValueUsesSamples(999'999, QStringLiteral("fadeIn")));
}

TEST(EditTimeValue, PlayheadRejectsTypedOutOfBoundsAndEmitsOneValidSeek) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto revision = session.workspace().revision();
    int seeks = 0;
    qint64 requested = -1;
    QObject::connect(&session, &EditSessionAdapter::seekRequestedUs, [&](qint64 value) {
        ++seeks;
        requested = value;
    });
    EXPECT_FALSE(session.applyTimeValue(0, QStringLiteral("playhead"), -1));
    EXPECT_FALSE(session.applyTimeValue(0, QStringLiteral("playhead"), 10'000'001));
    EXPECT_EQ(seeks, 0);
    ASSERT_TRUE(session.applyTimeValue(0, QStringLiteral("playhead"), 1'234'567));
    EXPECT_EQ(requested, 1'234'567);
    EXPECT_EQ(seeks, 1);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_FALSE(session.timeValueUsesSamples(0, QStringLiteral("playhead")));
}

TEST(EditTimeFieldAdapter, SharedParserPreservesExactStorageAndFormatsAllThreeModes) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto decimal = session.parseTimeValue(QStringLiteral("1,234567s"), 0);
    ASSERT_TRUE(decimal.value("valid").toBool());
    EXPECT_EQ(decimal.value("valueUs").toLongLong(), 1'234'567);
    EXPECT_FALSE(decimal.value("gridLabel").toString().isEmpty());
    const auto relative = session.parseTimeValue(QStringLiteral("+1f"), 16'666);
    ASSERT_TRUE(relative.value("valid").toBool());
    EXPECT_EQ(relative.value("valueUs").toLongLong(), 33'333);
    const auto samples = session.parseTimeValue(QStringLiteral("3"), 0, true);
    ASSERT_TRUE(samples.value("valid").toBool());
    EXPECT_EQ(samples.value("valueUs").toLongLong(), 63);
    const auto invalid = session.parseTimeValue(QStringLiteral("0.0000001s"), 123);
    EXPECT_FALSE(invalid.value("valid").toBool());
    EXPECT_EQ(invalid.value("valueUs").toLongLong(), 123);
    EXPECT_FALSE(invalid.value("error").toString().isEmpty());
    EXPECT_EQ(session.formatTimeValue(1'234'567, 1), QStringLiteral("1.234567"));
    EXPECT_EQ(session.formatTimeValue(1'000'000, 0), QStringLiteral("00:00:01:00"));
    EXPECT_EQ(session.formatTimeValue(63, 2), QStringLiteral("3"));
}

TEST(EditGainFieldAdapter, DecimalCommaUnitsBoundsAndTenthsShareOneStrictParser) {
    EnsureApplication();
    EditSessionAdapter session;
    const auto comma = session.parseGainValue(QStringLiteral("-12,3 dB"));
    ASSERT_TRUE(comma.value("valid").toBool());
    EXPECT_DOUBLE_EQ(comma.value("value").toDouble(), -12.3);
    EXPECT_DOUBLE_EQ(session.parseGainValue(QStringLiteral("+24.0")).value("value").toDouble(), 24);
    EXPECT_TRUE(session.parseGainValue(QStringLiteral("12"), true).value("valid").toBool());
    for (const auto& invalid : {QStringLiteral("24.1"), QStringLiteral("-60.1"), QStringLiteral("0.01"),
                                QStringLiteral("1,000"), QStringLiteral("1e1"), QStringLiteral("NaN")}) {
        const auto parsed = session.parseGainValue(invalid);
        EXPECT_FALSE(parsed.value("valid").toBool()) << invalid.toStdString();
        EXPECT_FALSE(parsed.value("error").toString().isEmpty());
    }
    EXPECT_FALSE(session.parseGainValue(QStringLiteral("12.1"), true).value("valid").toBool());
    EXPECT_EQ(session.formatGainValue(-12.3), QStringLiteral("-12.3"));
    EXPECT_EQ(session.formatGainValue(0), QStringLiteral("0.0"));
}

TEST(EditGainFieldAdapter, ExplicitMultiTargetGainIsAtomicAndDoesNotExpandLinksOrSkipInvalidIds) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.setEditContext(MakeContext(10));
    QVariantList ids;
    for (const auto& clip : session.workspace().clips())
        if (clip.track == session.workspace().tracks().back().id)
            ids.push_back(QVariant::fromValue<qulonglong>(clip.id));
    ASSERT_EQ(ids.size(), 2);
    const auto before = session.workspace().snapshot();
    ASSERT_TRUE(session.setClipGains(ids, -12.3));
    for (const auto& clip : session.workspace().clips())
        EXPECT_DOUBLE_EQ(clip.gain_db, clip.track == session.workspace().tracks().back().id ? -12.3 : 0);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), before->clips);
    const auto revision = session.workspace().revision();
    ids.push_back(QVariant::fromValue<qulonglong>(session.workspace().clips().front().id));
    EXPECT_FALSE(session.setClipGains(ids, -6));
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(session.workspace().clips(), before->clips);
}

TEST(EditPendingDraft, InvalidValueBlocksSelectionAndGesturesAndCloseCancelsTheOwner) {
    EnsureApplication();
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(import QtQml
QtObject {
    property bool cancelled: false
    function commitPendingDraft(): bool { return false; }
    function cancelPendingDraft(reason: string): void { cancelled = reason.length > 0; }
})",
                      QUrl());
    std::unique_ptr<QObject> owner(component.create());
    ASSERT_NE(owner, nullptr) << component.errorString().toStdString();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto selected = session.selectedClipIds();
    const auto clip = session.selectedClip();
    session.setPendingDraftOwner(owner.get(), clip);
    EXPECT_FALSE(session.preflightPendingDraft());
    session.clearSelection();
    EXPECT_EQ(session.selectedClipIds(), selected);
    session.beginClipDrag(clip);
    EXPECT_FALSE(session.dragActive());
    session.close();
    EXPECT_TRUE(owner->property("cancelled").toBool());
}

TEST(EditPendingDraft, ValidOwnerAppliesItsCommandOnceThroughTheReentryGuard) {
    EnsureApplication();
    QQmlEngine engine;
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto audio = session.workspace().clips().back().id;
    const auto revision = session.workspace().revision();
    engine.rootContext()->setContextProperty(QStringLiteral("session"), &session);
    QQmlComponent component(&engine);
    component.setData(R"(import QtQml
QtObject {
    property double target: 0
    property int commits: 0
    function commitPendingDraft(): bool {
        commits += 1;
        return session.setClipGain(target, -6);
    }
    function cancelPendingDraft(reason: string): void {}
})",
                      QUrl());
    std::unique_ptr<QObject> owner(component.create());
    ASSERT_NE(owner, nullptr) << component.errorString().toStdString();
    owner->setProperty("target", QVariant::fromValue<qulonglong>(audio));
    session.setPendingDraftOwner(owner.get(), audio);
    session.clearSelection();
    EXPECT_TRUE(session.selectedClipIds().isEmpty());
    EXPECT_DOUBLE_EQ(session.workspace().clip(audio)->gain_db, -6);
    EXPECT_EQ(session.workspace().revision(), revision + 1);
    EXPECT_EQ(owner->property("commits").toInt(), 1);
    EXPECT_TRUE(session.preflightPendingDraft());
    EXPECT_EQ(owner->property("commits").toInt(), 1);
}

TEST(EditClipboardAdapter, NonClipCopyKeepsClipboardAndPasteSelectsNewIdsWithoutSeeking) {
    EnsureApplication();
    EditSessionAdapter session;
    engine::EditMediaMetadata metadata;
    metadata.duration_us = 10'000'000;
    metadata.fps_num = 60;
    metadata.fps_den = 1;
    metadata.has_audio = true;
    metadata.content_identity_valid = true;
    metadata.content_sha256.fill(27);
    engine::EditSourceStream video;
    video.kind = engine::TimelineStreamKind::Video;
    video.container_index = 0;
    video.fps_num = 60;
    video.fps_den = 1;
    engine::EditSourceStream audio;
    audio.kind = engine::TimelineStreamKind::Audio;
    audio.container_index = 1;
    audio.codec.channels = 2;
    metadata.streams = {video, audio};
    session.addProbedMediaForHarness(QStringLiteral("clipboard-source.mkv"), metadata, true);
    ASSERT_EQ(session.workspace().clips().size(), 2u);
    session.selectAllClips();
    const auto original = session.workspace().clips();
    ASSERT_TRUE(session.copyClips());
    ASSERT_TRUE(session.hasClipClipboard());
    QVariantList tracks;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, tracks));
    EXPECT_FALSE(session.copyClips());
    EXPECT_FALSE(session.cutClips());
    EXPECT_TRUE(session.hasClipClipboard());
    EXPECT_EQ(session.workspace().clips(), original);
    int seeks = 0;
    QObject::connect(&session, &EditSessionAdapter::seekRequestedUs, [&](qint64) { ++seeks; });
    session.setActiveTrack(session.workspace().tracks().front().id);
    ASSERT_TRUE(session.previewPasteClipsUs(20'000'000));
    ASSERT_TRUE(session.dragValid());
    EXPECT_EQ(session.workspace().clips(), original);
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::ClipSelection);
    ASSERT_EQ(session.selectedClipIds().size(), 2);
    for (const auto& value : session.selectedClipIds())
        EXPECT_EQ(session.workspace().clip(value.toULongLong())->start, 20'000'000);
    EXPECT_EQ(seeks, 0);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), original);
}

TEST(EditTrackAudition, SoloIsAdditiveResidentAndAvailableOnLockedAudioTracks) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto first_audio = session.workspace().tracks().back().id;
    const auto audio_clip = session.workspace().clips().back().id;
    session.linkedSelection = false;
    session.beginClipDrag(audio_clip);
    session.updateClipDragUs(0, 0, 2, false, 0);
    ASSERT_TRUE(session.commitClipDrag());
    const auto second_audio = session.workspace().clip(audio_clip)->track;
    ASSERT_NE(first_audio, second_audio);
    ASSERT_TRUE(session.setTrackLocked(second_audio, true));
    const auto revision = session.workspace().revision();
    const auto history = session.workspace().historyBytes();
    int seeks = 0;
    int changes = 0;
    QObject::connect(&session, &EditSessionAdapter::seekRequestedUs, [&](qint64) { ++seeks; });
    QObject::connect(&session, &EditSessionAdapter::auditionChanged, [&] { ++changes; });
    ASSERT_TRUE(session.setTrackSolo(first_audio, true));
    ASSERT_TRUE(session.setTrackSolo(second_audio, true));
    EXPECT_EQ(session.soloTrackIds().size(), 2);
    EXPECT_TRUE(session.trackSolo(first_audio));
    EXPECT_TRUE(session.trackSolo(second_audio));
    EXPECT_FALSE(session.setTrackSolo(session.workspace().tracks().front().id, true));
    ASSERT_TRUE(session.setTrackSolo(first_audio, false));
    EXPECT_EQ(session.soloTrackIds().size(), 1);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(session.workspace().historyBytes(), history);
    EXPECT_EQ(seeks, 0);
    EXPECT_EQ(changes, 3);
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

TEST(EditViewport, FitIncludesTheTailAndSurvivesResizeAndRapidZoomReversal) {
    EditViewportAdapter view;
    view.setUsableWidth(1000);
    view.setTimelineEndUs(7200000000LL);
    view.fitTimeline();
    EXPECT_EQ(view.startUs(), 0);
    EXPECT_EQ(view.timeAtX(1000), 7'344'000'000LL);
    view.setUsableWidth(600);
    EXPECT_EQ(view.timeAtX(600), 7'344'000'000LL);
    for (int i = 0; i < 200; ++i) {
        view.zoomAt(96000, 300);
        view.panPixels(1e9);
        view.zoomAt(0.00001, 300);
    }
    EXPECT_EQ(view.startUs(), 0);
    EXPECT_EQ(view.timeAtX(600), 7'344'000'000LL);
    view.fitSelection(1000000, 2000000);
    EXPECT_EQ(view.startUs(), 980000);
    EXPECT_EQ(view.timeAtX(600), 2020000);
    view.restorePreviousView();
    EXPECT_EQ(view.startUs(), 0);
    EXPECT_EQ(view.timeAtX(600), 7'344'000'000LL);
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
    EXPECT_EQ(view.startUs(), 980000);
    EXPECT_EQ(view.endUs(), 2020000);
}

TEST(EditViewport, DurationPublicationDoesNotRescaleACompletedFit) {
    EditViewportAdapter view;
    view.setUsableWidth(600);
    view.setTimelineEndUs(10'000'000);
    view.fitTimeline();
    EXPECT_DOUBLE_EQ(view.pixelsPerSecond(), 50);
    view.setTimelineEndUs(20'000'000);
    EXPECT_DOUBLE_EQ(view.pixelsPerSecond(), 50);
    EXPECT_EQ(view.endUs(), 12'000'000);
    view.fitTimeline();
    EXPECT_EQ(view.endUs(), 22'000'000);
    view.zoomAt(600, 0);
    view.panPixels(1e9);
    EXPECT_EQ(view.startUs(), 21'000'000);
    EXPECT_EQ(view.endUs(), 22'000'000);
}

TEST(EditViewport, PreviousViewPopsCompletedZoomsAndIgnoresPans) {
    EditViewportAdapter view;
    view.setUsableWidth(600);
    view.setTimelineEndUs(100'000'000);
    view.setVisibleInterval(0, 5'000'000);
    view.zoomAt(240, 0);
    view.panPixels(100);
    view.restorePreviousView();
    EXPECT_EQ(view.startUs(), 0);
    EXPECT_EQ(view.endUs(), 5'000'000);
    for (int index = 1; index <= 25; ++index)
        view.zoomAt(120 * std::pow(1.1, index), 0);
    for (int index = 24; index >= 5; --index) {
        view.restorePreviousView();
        EXPECT_NEAR(view.pixelsPerSecond(), 120 * std::pow(1.1, index), 0.01);
    }
    const auto oldest = view.pixelsPerSecond();
    view.restorePreviousView();
    EXPECT_DOUBLE_EQ(view.pixelsPerSecond(), oldest);
}

TEST(EditViewport, CapturedPanDoesNotCreateZoomHistory) {
    EditViewportAdapter view;
    view.setUsableWidth(600);
    view.setTimelineEndUs(100'000'000);
    view.setVisibleInterval(0, 5'000'000);
    const auto depth = view.historyDepth();
    view.beginViewOperation();
    view.panPixels(100);
    view.endViewOperation();
    EXPECT_EQ(view.historyDepth(), depth);
}

TEST(EditViewport, ExplicitCaptureIgnoresIdleAndWheelBurstsCompleteOnce) {
    EnsureApplication();
    EditViewportAdapter view;
    view.setUsableWidth(600);
    view.setTimelineEndUs(100'000'000);
    view.setVisibleInterval(0, 5'000'000);
    const auto depth = view.historyDepth();
    view.beginViewOperation();
    view.zoomAt(240, 0);
    view.zoomAt(480, 0);
    QEventLoop idle;
    QTimer::singleShot(350, &idle, &QEventLoop::quit);
    idle.exec();
    EXPECT_EQ(view.historyDepth(), depth);
    view.endViewOperation();
    EXPECT_EQ(view.historyDepth(), depth + 1);
    view.restorePreviousView();
    EXPECT_EQ(view.startUs(), 0);
    EXPECT_EQ(view.endUs(), 5'000'000);
    view.zoomWheelAt(240, 0);
    view.zoomWheelAt(480, 0);
    EXPECT_EQ(view.historyDepth(), depth);
    QTimer::singleShot(350, &idle, &QEventLoop::quit);
    idle.exec();
    EXPECT_EQ(view.historyDepth(), depth + 1);
    view.restorePreviousView();
    EXPECT_EQ(view.endUs(), 5'000'000);
}

TEST(EditOverview, BodyPanAndOutsideCenterPreserveSpanWithoutZoomHistory) {
    EditViewportAdapter view;
    view.setTimelineEndUs(100'000'000);
    view.setUsableWidth(1000);
    view.setVisibleInterval(20'000'000, 40'000'000);
    const auto depth = view.historyDepth();
    ASSERT_TRUE(view.beginOverviewOperation(300, 1020));
    EXPECT_EQ(view.overviewTarget(), 1);
    view.updateOverviewOperation(400, 1020);
    view.endOverviewOperation();
    EXPECT_EQ(view.startUs(), 30'000'000);
    EXPECT_EQ(view.endUs(), 50'000'000);
    EXPECT_EQ(view.historyDepth(), depth);
    view.centerOverviewAt(800, 1020);
    EXPECT_EQ(view.startUs(), 70'000'000);
    EXPECT_EQ(view.endUs(), 90'000'000);
    EXPECT_EQ(view.historyDepth(), depth);
}

TEST(EditOverview, EdgeZoomAnchorsOppositeEdgeAndCompletesHistoryOnlyAtRelease) {
    EnsureApplication();
    EditViewportAdapter view;
    view.setTimelineEndUs(100'000'000);
    view.setUsableWidth(1000);
    view.setVisibleInterval(20'000'000, 40'000'000);
    const auto depth = view.historyDepth();
    ASSERT_TRUE(view.beginOverviewOperation(200, 1020));
    EXPECT_EQ(view.overviewTarget(), 2);
    view.updateOverviewOperation(100, 1020);
    EXPECT_EQ(view.startUs(), 10'000'000);
    EXPECT_EQ(view.endUs(), 40'000'000);
    EXPECT_EQ(view.historyDepth(), depth);
    QEventLoop idle;
    QTimer::singleShot(350, &idle, &QEventLoop::quit);
    idle.exec();
    EXPECT_EQ(view.historyDepth(), depth);
    view.endOverviewOperation();
    EXPECT_EQ(view.historyDepth(), depth + 1);
    view.restorePreviousView();
    EXPECT_EQ(view.startUs(), 20'000'000);
    EXPECT_EQ(view.endUs(), 40'000'000);
}

TEST(EditOverview, OverlappingEdgeHitsUseNearestThenLastActiveEdge) {
    EditViewportAdapter view;
    view.setTimelineEndUs(100'000'000);
    view.setUsableWidth(1000);
    view.setVisibleInterval(20'000'000, 20'400'000);
    ASSERT_TRUE(view.beginOverviewOperation(204, 1020));
    EXPECT_EQ(view.overviewTarget(), 3);
    view.endOverviewOperation();
    ASSERT_TRUE(view.beginOverviewOperation(202, 1020));
    EXPECT_EQ(view.overviewTarget(), 3);
    view.endOverviewOperation();
    ASSERT_TRUE(view.beginOverviewOperation(200, 1020));
    EXPECT_EQ(view.overviewTarget(), 2);
    view.endOverviewOperation();
    ASSERT_TRUE(view.beginOverviewOperation(202, 1020));
    EXPECT_EQ(view.overviewTarget(), 2);
    view.endOverviewOperation();
}

TEST(EditOverview, InvalidGeometryDoesNotCaptureOrMoveViewport) {
    EditViewportAdapter view;
    const auto before = view.startUs();
    EXPECT_FALSE(view.beginOverviewOperation(0, 0));
    EXPECT_FALSE(view.beginOverviewOperation(std::numeric_limits<qreal>::quiet_NaN(), 100));
    view.centerOverviewAt(0, 0);
    view.updateOverviewOperation(100, 100);
    EXPECT_EQ(view.startUs(), before);
    EXPECT_EQ(view.overviewTarget(), 0);
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

TEST(EditPlaybackPublication, AudioControlsRetainSourceLayoutButVideoAndPlacementChangesDoNot) {
    engine::TimelineSnapshot before;
    before.duration_us = 10'000'000;
    before.tracks = {{1, false}, {2, true}};
    before.clips = {{1, 1, 1, false, 0, 0, 10'000'000, 1}, {2, 1, 1, true, 0, 0, 10'000'000, 2}};
    auto after = before;
    after.revision = 2;
    after.clips[1].enabled = false;
    after.clips[1].gain_db = -12;
    after.tracks[1].enabled = false;
    after.tracks[1].gain_db = 6;
    EXPECT_TRUE(SamePlaybackMediaLayout(before, after));
    after.clips[0].enabled = false;
    EXPECT_FALSE(SamePlaybackMediaLayout(before, after));
    after = before;
    after.clips[1].start_us = 1;
    EXPECT_FALSE(SamePlaybackMediaLayout(before, after));
    after = before;
    after.clips[0].opacity = 0.5;
    EXPECT_FALSE(SamePlaybackMediaLayout(before, after));
}

TEST(EditTransportBounds, LoopLatchesCommittedRangeAndClearFallsBackToWholeTimeline) {
    edit::TransportBounds bounds;
    bounds.setDuration(10'000'000);
    bounds.setLoop(true, edit::PlaybackInterval{2'000'000, 4'000'000});
    EXPECT_TRUE(bounds.repeating());
    EXPECT_EQ(bounds.interval(), (edit::PlaybackInterval{2'000'000, 4'000'000}));
    bounds.commitRange({5'000'000, 7'000'000});
    EXPECT_EQ(bounds.loopRegion(), (edit::PlaybackInterval{5'000'000, 7'000'000}));
    bounds.clearRange();
    EXPECT_TRUE(bounds.loopEnabled());
    EXPECT_FALSE(bounds.loopRegion());
    EXPECT_EQ(bounds.interval(), (edit::PlaybackInterval{0, 10'000'000}));
}

TEST(EditTransportBounds, OnceKeepsCopiedBoundsAcrossLoopAndRangeChanges) {
    edit::TransportBounds bounds;
    bounds.setDuration(10'000'000);
    bounds.setLoop(true, edit::PlaybackInterval{1'000'000, 8'000'000});
    ASSERT_TRUE(bounds.beginOnce({2'000'000, 3'000'000}));
    EXPECT_FALSE(bounds.repeating());
    bounds.commitRange({5'000'000, 7'000'000});
    bounds.setLoop(false);
    EXPECT_EQ(bounds.interval(), (edit::PlaybackInterval{2'000'000, 3'000'000}));
    bounds.setLoop(true, edit::PlaybackInterval{5'000'000, 7'000'000});
    bounds.completeOnce();
    EXPECT_FALSE(bounds.onceActive());
    EXPECT_TRUE(bounds.repeating());
    EXPECT_EQ(bounds.interval(), (edit::PlaybackInterval{5'000'000, 7'000'000}));
}

TEST(EditTransportBounds, OutsideSeekExitsModesAndShrinkNeverKeepsAnEmptyRegion) {
    edit::TransportBounds bounds;
    bounds.setDuration(10'000'000);
    bounds.setLoop(true, edit::PlaybackInterval{2'000'000, 8'000'000});
    ASSERT_TRUE(bounds.beginOnce({3'000'000, 6'000'000}));
    bounds.seek(4'000'000);
    EXPECT_TRUE(bounds.onceActive());
    bounds.seek(6'000'000);
    EXPECT_FALSE(bounds.onceActive());
    EXPECT_FALSE(bounds.loopEnabled());
    bounds.setLoop(true, edit::PlaybackInterval{2'000'000, 8'000'000});
    ASSERT_TRUE(bounds.beginOnce({3'000'000, 6'000'000}));
    bounds.setDuration(5'000'000);
    EXPECT_EQ(bounds.onceBounds(), (edit::PlaybackInterval{3'000'000, 5'000'000}));
    bounds.setDuration(1'000'000);
    EXPECT_FALSE(bounds.onceActive());
    EXPECT_FALSE(bounds.loopRegion());
    EXPECT_EQ(bounds.interval(), (edit::PlaybackInterval{0, 1'000'000}));
    EXPECT_FALSE(bounds.beginOnce({1, 1}));
}

TEST(EditTrackLayout, TypeDefaultsAndCollapsedRowsHaveNoMediaBody) {
    EditTrackLayoutModel layout;
    layout.setTracks({QVariantMap{{"id", 10}, {"video", true}}, QVariantMap{{"id", 20}, {"video", false}}});
    EXPECT_EQ(layout.heightForId(10), 64);
    EXPECT_EQ(layout.heightForId(20), 44);
    EXPECT_EQ(layout.rows()[1].toMap().value("headerHeight").toReal(), 16);
    layout.setTrackHeight(10, 22);
    EXPECT_EQ(layout.heightForId(10), 22);
    EXPECT_EQ(layout.rows()[0].toMap().value("headerHeight").toReal(), 22);
    EXPECT_EQ(layout.rows()[0].toMap().value("contentHeight").toReal(), 0);
    layout.setTrackHeight(10, 43);
    EXPECT_EQ(layout.rows()[0].toMap().value("headerHeight").toReal(), 43);
    EXPECT_EQ(layout.rows()[0].toMap().value("contentHeight").toReal(), 0);
}

TEST(EditTrackLayout, CollapseRestoresExpandedHeightAndColorSurvivesReorderUntilReset) {
    EditTrackLayoutModel layout;
    const QVariantMap video{{"id", 10}, {"video", true}};
    const QVariantMap audio{{"id", 20}, {"video", false}};
    layout.setTracks({video, audio});
    layout.setTrackHeight(20, 112);
    layout.setTrackColor(20, QStringLiteral("#5d84a0"));
    layout.toggleCollapsed(20);
    EXPECT_EQ(layout.heightForId(20), 22);
    layout.setTracks({audio, video});
    EXPECT_EQ(layout.rows()[0].toMap().value("color").toString(), QStringLiteral("#5d84a0"));
    layout.toggleCollapsed(20);
    EXPECT_EQ(layout.heightForId(20), 112);
    layout.setTrackHeight(20, 28);
    layout.toggleCollapsed(20);
    EXPECT_EQ(layout.heightForId(20), 112);
    layout.setTrackColor(20, QStringLiteral("not a color"));
    EXPECT_EQ(layout.rows()[0].toMap().value("color").toString(), QStringLiteral("#5d84a0"));
    layout.reset();
    layout.setTracks({audio});
    EXPECT_EQ(layout.heightForId(20), 44);
    EXPECT_TRUE(layout.rows()[0].toMap().value("color").toString().isEmpty());
}

TEST(EditTrackLayout, UnequalHeightsShareHitAndDropBoundaries) {
    EditTrackLayoutModel layout;
    const QVariantList tracks{QVariantMap{{"id", 10}, {"video", true}}, QVariantMap{{"id", 20}, {"video", false}},
                              QVariantMap{{"id", 30}, {"video", false}}};
    layout.setTracks(tracks);
    layout.setTrackHeight(10, 28);
    layout.setTrackHeight(20, 120);
    EXPECT_EQ(layout.totalHeight(), 192);
    EXPECT_EQ(layout.topForId(20), 28);
    EXPECT_EQ(layout.topForId(30), 148);
    EXPECT_EQ(layout.trackAtY(27.9), 10);
    EXPECT_EQ(layout.trackAtY(28), 20);
    EXPECT_EQ(layout.trackAtY(147.9), 20);
    EXPECT_EQ(layout.trackAtY(148), 30);
    EXPECT_EQ(layout.trackAtY(192), 0);
    EXPECT_EQ(layout.trackAtY(-1), 0);
    EXPECT_EQ(layout.dropTargetAtY(-1).value("newTrack").toInt(), 0);
    EXPECT_EQ(layout.dropTargetAtY(21).value("newTrack").toInt(), 1);
    EXPECT_EQ(layout.dropTargetAtY(37).value("trackId").toULongLong(), 20);
    EXPECT_EQ(layout.dropTargetAtY(192).value("newTrack").toInt(), 2);
    layout.setTrackHeight(10, 1);
    EXPECT_EQ(layout.heightForId(10), 22);
    layout.setTrackHeight(20, 300);
    EXPECT_EQ(layout.heightForId(20), 240);
}

TEST(EditTrackLayout, PreviewInheritsSizesWithoutChangingBaseDropTargets) {
    EditTrackLayoutModel layout;
    const QVariantMap video{{"id", 10}, {"video", true}};
    const QVariantMap audio{{"id", 20}, {"video", false}};
    layout.setTracks({video, audio});
    layout.setTrackHeight(10, 28);
    layout.setTrackHeight(20, 120);
    layout.setPreviewTracks({QVariantMap{{"id", 40}, {"video", true}}, video, audio});
    EXPECT_EQ(layout.topForId(10), 0);
    EXPECT_EQ(layout.topForId(10, true), 64);
    EXPECT_EQ(layout.heightForId(10, true), 28);
    EXPECT_EQ(layout.topForId(20, true), 92);
    EXPECT_EQ(layout.totalHeight(), 212);
    EXPECT_EQ(layout.dropTargetAtY(148).value("newTrack").toInt(), 2);
    layout.setTracks({audio});
    layout.setTracks({audio, video});
    EXPECT_EQ(layout.topForId(10), 120);
    EXPECT_EQ(layout.heightForId(10), 28);
}

TEST(EditTrackLayout, ClosingWorkspaceClearsResidentSizesBeforeIdsAreReused) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto track = session.tracks().front().toMap().value("id").toULongLong();
    session.trackLayout()->setTrackHeight(track, 28);
    session.close();
    session.setEditContext(MakeContext(10));
    ASSERT_EQ(session.tracks().front().toMap().value("id").toULongLong(), track);
    EXPECT_EQ(session.trackLayout()->heightForId(track), 64);
}

TEST(EditWorkspacePrecision, FrameNudgeAndTrimDoNotRoundThroughMillisecondValues) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.nudgeSelectedUs(16'666);
    EXPECT_EQ(session.workspace().clips().front().start, 16'666);
    session.nudgeSelectedUs(16'667, 1);
    EXPECT_EQ(session.workspace().clips().front().start, 33'333);
    EXPECT_EQ(session.workspace().clips().front().source_in, 16'667);
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

TEST(EditWorkspaceAdapter, AssetAndRecordingPlacementPreserveFractionalMilliseconds) {
    EnsureApplication();
    for (const bool recording : {false, true}) {
        SCOPED_TRACE(recording ? "recording" : "asset");
        EditSessionAdapter session;
        session.setEditContext(MakeContext(1));
        const auto asset = session.workspace().assets().front().id;
        session.undo();
        if (recording)
            session.setEditContextUs(MakeContext(1), 21);
        else
            session.appendAssetUs(asset, 21);
        ASSERT_EQ(session.workspace().clips().size(), 2u);
        for (const auto& clip : session.workspace().clips())
            EXPECT_EQ(clip.start, 21);
        EXPECT_EQ(session.durationUs(), 1'000'021);
        session.undo();
        EXPECT_TRUE(session.workspace().clips().empty());
        session.redo();
        for (const auto& clip : session.workspace().clips())
            EXPECT_EQ(clip.start, 21);
    }
}

TEST(EditWorkspaceAdapter, HistoryPlacementEmitsExactMicroseconds) {
    EnsureApplication();
    EditSessionAdapter session;
    qint64 requested = -2;
    QObject::connect(&session, &EditSessionAdapter::historyRequestedUs, &session,
                     [&](const QString&, qint64 at_us) { requested = at_us; });
    session.addHistoryUs(QStringLiteral("recording.mkv"), 21);
    EXPECT_EQ(requested, 21);
    session.addHistory(QStringLiteral("recording.mkv"), 2);
    EXPECT_EQ(requested, 2000);
}

TEST(EditWorkspaceAdapter, CrossfadeDropDoesNotRoundACutIntoTheTolerance) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto first = session.workspace().clips().front().id;
    // A split keeps source handles on both sides of the cut, which a crossfade
    // needs. One microsecond before the frame-aligned cut must not round onto it.
    session.selectClip(first);
    session.setPositionUs(5'000'000);
    session.splitSelected();
    const auto cut = session.workspace().clip(first)->end();
    ASSERT_TRUE(session.canCrossfadeAtCut(first));
    const auto revision = session.workspace().revision();
    EXPECT_FALSE(session.dropCrossfadeUs(cut - 1, 0));
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_TRUE(session.dropCrossfadeUs(cut, 0)) << session.workspaceError().toStdString();
}

TEST(EditWorkspaceAdapter, AsyncMediaAndRecordingPreserveFractionalPlacement) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const auto path = directory.filePath(QStringLiteral("source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, path));
    for (const bool recording : {false, true}) {
        SCOPED_TRACE(recording ? "recording" : "media");
        EditSessionAdapter session;
        QEventLoop loop;
        QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &loop, &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        if (recording) {
            auto context = MakeContext(0);
            context.output_path = path;
            context.mkv_master_path = path;
            session.setEditContextUs(context, 21);
        } else {
            session.importMediaBatchUs({QUrl::fromLocalFile(path)}, true, 21);
        }
        loop.exec();
        ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
        ASSERT_EQ(session.workspace().clips().size(), 1u);
        EXPECT_EQ(session.workspace().clips().front().start, 21);
        session.undo();
        EXPECT_TRUE(session.workspace().clips().empty());
        session.redo();
        EXPECT_EQ(session.workspace().clips().front().start, 21);
    }
}

TEST(EditWorkspaceAdapter, MillisecondPlacementRejectsOverflowBeforeImportOrContextChange) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(1));
    const auto clips = session.workspace().clips();
    const auto revision = session.workspace().revision();
    const auto path = session.editContext().output_path;
    auto incoming = MakeContext(1);
    incoming.output_path = QStringLiteral("another-recording.mkv");
    const auto maximum = (std::numeric_limits<qint64>::max)();
    session.setEditContext(incoming, maximum);
    EXPECT_EQ(session.editContext().output_path, path);
    EXPECT_EQ(session.workspace().clips(), clips);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_FALSE(session.workspaceError().isEmpty());
    int requested = 0;
    QObject::connect(&session, &EditSessionAdapter::historyRequestedUs, &session,
                     [&](const QString&, qint64) { ++requested; });
    session.addHistory(QStringLiteral("recording.mkv"), maximum);
    EXPECT_EQ(requested, 0);
    session.appendAsset(session.workspace().assets().front().id, maximum);
    session.importMediaBatch({}, true, maximum);
    EXPECT_FALSE(session.importPending());
    EXPECT_EQ(session.workspace().clips(), clips);
    EXPECT_EQ(session.workspace().revision(), revision);
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

TEST(EditWorkspaceAdapter, SavedStandaloneClipRemainsImportableAndEditable) {
    EnsureApplication();
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

TEST(EditWorkspaceAdapter, AnUnchangedMarqueeDoesNotRepublishTheWorkspace) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    session.clearSelection();
    const auto clip = session.workspace().clips().front();
    const QVariantList tracks{QVariant::fromValue<qulonglong>(clip.track)};
    int publications = 0;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, [&] { ++publications; });
    session.selectClipArea(clip.start, clip.end(), tracks, {}, true);
    ASSERT_EQ(publications, 1);
    const auto selected = session.selectedClipIds();
    session.selectClipArea(clip.start, clip.end(), tracks, {}, true);
    EXPECT_EQ(publications, 1);
    EXPECT_EQ(session.selectedClipIds(), selected);
}

TEST(EditExportAdapterRun, RetryPreservesTheFailedDestinationAndItsReplaceConsent) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto destination = directory.filePath(QStringLiteral("existing.mkv"));
    QFile file(destination);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("keep", 4), 4);
    file.close();
    EditSessionAdapter session;
    auto context = MakeContext();
    context.output_path = directory.filePath(QStringLiteral("missing-source.mkv"));
    session.setEditContext(context);
    EditExportAdapter exporter;
    exporter.setSession(&session);
    EditExportAdapterTestPeer::chooseOutput(exporter, std::filesystem::path(destination.toStdWString()), true);
    const auto await_failure = [&] {
        QElapsedTimer timer;
        timer.start();
        while (exporter.state() != EditExportAdapter::Failed && timer.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        return exporter.state() == EditExportAdapter::Failed;
    };
    exporter.startExport();
    ASSERT_TRUE(await_failure());
    ASSERT_FALSE(exporter.errorText().contains(QStringLiteral("confirm before replacing")));
    exporter.retry();
    EXPECT_EQ(exporter.state(), EditExportAdapter::Preparing) << exporter.errorText().toStdString();
    ASSERT_TRUE(await_failure());
    EXPECT_EQ(QDir::fromNativeSeparators(exporter.outputPath()), destination);
    EXPECT_FALSE(exporter.errorText().contains(QStringLiteral("confirm before replacing")));
    EditExportAdapterTestPeer::chooseOutput(exporter, std::filesystem::path(destination.toStdWString()), false);
    exporter.startExport();
    ASSERT_EQ(exporter.state(), EditExportAdapter::Failed);
    exporter.retry();
    EXPECT_EQ(exporter.state(), EditExportAdapter::Failed);
    EXPECT_TRUE(exporter.errorText().contains(QStringLiteral("confirm before replacing")));
    const auto other_destination = directory.filePath(QStringLiteral("other.mkv"));
    QFile other(other_destination);
    ASSERT_TRUE(other.open(QIODevice::WriteOnly));
    ASSERT_EQ(other.write("other", 5), 5);
    other.close();
    EditExportAdapterTestPeer::chooseOutput(exporter, std::filesystem::path(other_destination.toStdWString()), true);
    exporter.startExport();
    EXPECT_EQ(exporter.state(), EditExportAdapter::Preparing);
    EXPECT_EQ(QDir::fromNativeSeparators(exporter.outputPath()), other_destination);
    ASSERT_TRUE(await_failure());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), QByteArray("keep"));
}

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

TEST(EditWorkspaceAdapter, SourceChannelCountComesFromTheBoundAudioStream) {
    EnsureApplication();
    EditSessionAdapter session;
    engine::EditMediaMetadata metadata;
    metadata.content_identity_valid = true;
    metadata.content_sha256.fill(37);
    metadata.duration_us = 2'000'000;
    metadata.has_audio = true;
    for (const auto channels : {1u, 2u}) {
        engine::EditSourceStream stream;
        stream.kind = engine::TimelineStreamKind::Audio;
        stream.container_index = channels == 1 ? 4 : 9;
        stream.codec.channels = channels;
        metadata.streams.push_back(stream);
    }
    session.addProbedMediaForHarness(QStringLiteral("source.mkv"), metadata, true);
    const auto rows = session.visibleClipsUs(0, metadata.duration_us);
    ASSERT_EQ(rows.size(), 2);
    for (const auto& value : rows) {
        const auto row = value.toMap();
        EXPECT_FALSE(row.value(QStringLiteral("video")).toBool());
        EXPECT_EQ(row.value(QStringLiteral("sourceChannels")).toUInt(),
                  row.value(QStringLiteral("containerIndex")).toInt() == 4 ? 1u : 2u);
    }
    EditSessionAdapter legacy;
    legacy.setEditContext(MakeContext());
    for (const auto& value : legacy.visibleClipsUs(0, legacy.durationUs())) {
        EXPECT_TRUE(value.toMap().contains(QStringLiteral("sourceChannels")));
        EXPECT_EQ(value.toMap().value(QStringLiteral("sourceChannels")).toUInt(), 0u);
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
        // Until verification proves the content, sourceIdentity is a per-lease
        // placeholder; cache reuse across imports is keyed by the proven digest.
        const auto asset_id = session.workspace().assets().front().id;
        ASSERT_TRUE(wait_for([&]() { return bool(session.sourceProof(asset_id)); }));
        EditTimelineAdapter timeline;
        timeline.configureWaveformCache(config);
        auto* waveforms = qobject_cast<WaveformCacheService*>(timeline.waveformService());
        ASSERT_NE(waveforms, nullptr);
        QString failure;
        QObject::connect(waveforms, &WaveformCacheService::failed,
                         [&](const QString&, const QString& reason) { failure = reason; });
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
        ASSERT_TRUE(wait_for([&]() { return waveforms->state(key) == WaveformCacheService::State::Ready; }))
            << failure.toStdString();
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
    // A split keeps the selected id on its left part, so each later cut selects
    // the video piece that contains it.
    for (const qint64 at : {500, 1000, 1500}) {
        const auto current = session.workspace().clips();
        const auto piece = std::find_if(current.begin(), current.end(), [&](const auto& clip) {
            return !is_audio(clip) && clip.start < at * 1000 && at * 1000 < clip.end();
        });
        ASSERT_NE(piece, current.end()) << at;
        session.selectClip(piece->id);
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
            // An unchanged picture is copied even when the finished stereo mix is rendered.
            EXPECT_EQ(reviewed.value(QStringLiteral("path")).toString(), QStringLiteral("Copy"));
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
    // Waveform keys use the proven content digest, not the per-lease placeholder
    // a row carries while verification is still running.
    for (const auto& asset : session.workspace().assets())
        ASSERT_TRUE(wait_for([&]() { return bool(session.sourceProof(asset.id)); }));
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
    // A time selection replaces the clip selection, so the selected-clips scope
    // is exported before the in/out boundaries are set.
    session.selectClip(blue_id);
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
    session.requestSeekUs(750'000);
    session.setSelectionBoundary(false);
    session.requestSeekUs(3'000'000);
    session.setSelectionBoundary(true);
    exporter.setTimeSelectionUs(session.timeSelectionInUs(), session.timeSelectionOutUs());
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
    EXPECT_FALSE(exporter.resultOutdated());
    auto bin_metadata = source_metadata.front();
    bin_metadata.content_sha256.fill(91);
    session.addProbedMediaForHarness(QStringLiteral("bin-only.mkv"), bin_metadata, false);
    EXPECT_FALSE(exporter.resultOutdated());
    session.moveClip(blue_id, 2600, false);
    EXPECT_TRUE(exporter.resultOutdated());
    session.undo();
    EXPECT_FALSE(exporter.resultOutdated());
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

TEST(EditExportSourceLease, ReviewReusesVerifiedLiveSourceWithoutAFullHash) {
    EnsureApplication();
    QTemporaryDir output;
    ASSERT_TRUE(output.isValid());
    const auto fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    EditSessionAdapter session;
    session.importMediaBatch({QUrl::fromLocalFile(fixture)}, true);
    QElapsedTimer deadline;
    deadline.start();
    while ((session.workspace().assets().empty() || !session.sourceProof(session.workspace().assets().front().id)) &&
           deadline.elapsed() < 10'000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_EQ(session.workspace().assets().size(), 1u);
    ASSERT_TRUE(session.sourceProof(session.workspace().assets().front().id));
    const auto before = engine::GlobalMediaHashMeasurements();
    EditExportAdapter exporter;
    exporter.setSession(&session);
    exporter.setModeKey(QStringLiteral("originalStreams"));
    exporter.setOutputDirectoryProvider([&] { return output.path(); });
    exporter.startExport();
    deadline.restart();
    while (exporter.state() == EditExportAdapter::Preparing && deadline.elapsed() < 10'000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_EQ(exporter.state(), EditExportAdapter::Review) << exporter.errorText().toStdString();
    const auto after = engine::GlobalMediaHashMeasurements();
    EXPECT_EQ(after.total.starts - before.total.starts, 0u);
    EXPECT_EQ(after.total.bytes - before.total.bytes, 0u);
    EXPECT_TRUE(QDir(output.path()).entryList(QDir::Files).isEmpty());
}

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
    std::optional<engine::EditMediaMetadata> excluded_metadata;
    if (selected_only) {
        excluded_metadata = engine::ProbeEditMedia(std::filesystem::path(second.toStdWString()));
        ASSERT_TRUE(excluded_metadata->error.empty()) << excluded_metadata->error;
        ASSERT_TRUE(excluded_metadata->content_identity_valid);
        ASSERT_TRUE(QFile::remove(second));
    }
    EditSessionAdapter session;
    QEventLoop imported;
    QObject::connect(&session, &EditSessionAdapter::workspaceChanged, &imported, &QEventLoop::quit);
    QTimer::singleShot(10000, &imported, &QEventLoop::quit);
    session.importMediaBatch(selected_only ? QList<QUrl>{QUrl::fromLocalFile(first)}
                                           : QList<QUrl>{QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)},
                             true);
    imported.exec();
    if (selected_only) {
        const auto excluded = session.addProbedMediaForHarness(second, *excluded_metadata, true);
        ASSERT_FALSE(session.sourceAccess(excluded));
        ASSERT_FALSE(QFile::exists(second));
    }
    ASSERT_TRUE(session.workspaceError().isEmpty()) << session.workspaceError().toStdString();
    ASSERT_EQ(session.workspace().assets().size(), 2u);
    if (use_render_fixture) {
        for (const auto& asset : session.workspace().assets()) {
            if (selected_only && asset.path == second.toStdWString())
                continue;
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
    std::vector<std::weak_ptr<const engine::MediaReadCapability>> owners;
    for (const auto& asset : session.workspace().assets())
        if (const auto access = session.sourceAccess(asset.id))
            owners.push_back(access);
    ASSERT_FALSE(owners.empty());
    for (const auto& path : {first, second}) {
        if (selected_only && path == second)
            continue;
        QFile writable(path);
        EXPECT_FALSE(writable.open(QIODevice::ReadWrite)) << path.toStdString();
    }
    session.close();
    const auto drained = [&] {
        return std::all_of(owners.begin(), owners.end(), [](const auto& owner) { return owner.expired(); });
    };
    QElapsedTimer drain_timer;
    drain_timer.start();
    while (!drained() && drain_timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    ASSERT_TRUE(drained());
    for (const auto& path : {first, second}) {
        if (selected_only && path == second)
            continue;
        QFile writable(path);
        ASSERT_TRUE(writable.open(QIODevice::ReadWrite)) << path.toStdString();
        writable.close();
        const auto retired_path = path + QStringLiteral(".released");
        ASSERT_TRUE(QFile::rename(path, retired_path));
        EXPECT_TRUE(QFile::remove(retired_path));
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

TEST(EditExportAdapterRun, ChangedBytesBeforePreparationRequireReimport) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto fixture_template =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const auto fixture = QDir(directory.path()).filePath(QStringLiteral("source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture_template, fixture));
    const auto original = engine::ProbeEditMedia(std::filesystem::path(fixture.toStdWString()));
    ASSERT_TRUE(original.error.empty()) << original.error;
    ASSERT_TRUE(original.content_identity_valid);
    EditSessionAdapter session;
    const auto asset = session.addProbedMediaForHarness(fixture, original, true);
    ASSERT_EQ(session.workspace().clips().size(), 1u);
    ASSERT_NE(session.workspace().asset(asset)->source_descriptor, nullptr);
    ASSERT_EQ(session.workspace().asset(asset)->source_descriptor->asset_identity_fingerprint, original.content_sha256);
    ASSERT_FALSE(session.sourceAccess(asset));
    QFile file(fixture);
    ASSERT_TRUE(file.open(QIODevice::Append));
    ASSERT_EQ(file.write("x", 1), 1);
    file.close();
    EditExportAdapter exporter;
    exporter.setSession(&session);
    exporter.setOutputDirectoryProvider([&] { return directory.path(); });
    QEventLoop prepared;
    QObject::connect(&exporter, &EditExportAdapter::stateChanged, &prepared, [&] {
        if (exporter.state() == EditExportAdapter::Review || exporter.state() == EditExportAdapter::Failed)
            prepared.quit();
    });
    QTimer::singleShot(10000, &prepared, &QEventLoop::quit);
    exporter.startExport();
    prepared.exec();
    EXPECT_EQ(exporter.state(), EditExportAdapter::Failed);
    EXPECT_TRUE(exporter.errorText().contains(QStringLiteral("Media content differs from expected identity")))
        << exporter.errorText().toStdString();
    EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files), QStringList{QStringLiteral("source.mp4")});
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
    // The imported source is held by a read lease that admits no writer, so its
    // bytes cannot change between review and confirmation. Changed bytes of an
    // unleased source are covered by ChangedBytesBeforePreparationRequireReimport.
    QFile source_file(fixture);
    EXPECT_FALSE(source_file.open(QIODevice::Append));
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

TEST(EditSessionDrag, CaptureKeepsExplicitMultiSelectionAndPressTimeLinkedPolicy) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    session.setEditContext(MakeContext(10));
    session.snapping = false;
    session.selectAllClips();
    const auto original = session.workspace().clips();
    const auto selected = session.selectedClipIds();
    ASSERT_EQ(selected.size(), 4);
    const auto lead = selected.front().toULongLong();
    session.beginClipDrag(lead);
    session.linkedSelection = false;
    session.updateClipDragUs(1'000'000);
    ASSERT_TRUE(session.dragValid());
    EXPECT_EQ(session.dragClips().size(), 4);
    session.clearSelection();
    session.cancelClipDrag();
    EXPECT_EQ(session.selectedClipIds(), selected);
    EXPECT_EQ(session.workspace().clips(), original);
    session.clearSelection();
    session.linkedSelection = true;
    session.selectClip(lead);
    session.beginClipDrag(lead);
    session.linkedSelection = false;
    session.updateClipDragUs(1'000'000);
    ASSERT_TRUE(session.dragValid());
    EXPECT_EQ(session.dragClips().size(), 2);
    session.cancelClipDrag();
}

TEST(EditRangeGesture, ScopeUsesCurrentTrackSpanAndAtomicallyRejectsCollapsedVideoConversion) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    const auto video = session.workspace().tracks().front().id;
    const auto audio = session.workspace().tracks().back().id;
    const QVariantList audio_scope{QVariant::fromValue<qulonglong>(audio)};
    const auto all_scope = session.rangeTrackSpan(video, audio);
    ASSERT_EQ(all_scope.size(), 2);
    ASSERT_TRUE(session.beginRangeGestureUs(1'000'000, audio_scope));
    session.updateRangeGestureToTrackUs(2'000'000, video);
    ASSERT_TRUE(session.rangeGestureValid());
    EXPECT_EQ(session.rangeGestureCandidate().value("trackIds").toList().size(), 2);
    session.updateRangeGestureToTrackUs(2'000'000, audio);
    ASSERT_TRUE(session.rangeGestureValid());
    EXPECT_EQ(session.rangeGestureCandidate().value("trackIds").toList(), audio_scope);
    session.cancelRangeGesture();
    ASSERT_TRUE(session.setEditRangeUs(21, 42, audio_scope));
    const auto original = session.editRange();
    ASSERT_TRUE(session.beginRangeGestureUs(21, audio_scope, true, true));
    session.updateRangeGestureToTrackUs(42, video);
    EXPECT_FALSE(session.rangeGestureValid());
    EXPECT_FALSE(session.commitRangeGesture());
    EXPECT_EQ(session.editRange(), original);
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
    session.clearSelection();
    session.selectClip(id);
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

TEST(EditMediaModel, SelectingClipsDoesNotReplaceTheAssetModel) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    auto* model = session.mediaModel();
    ASSERT_GT(model->rowCount(), 0);
    const QPersistentModelIndex first(model->index(0));
    int resets = 0;
    int media_changes = 0;
    QObject::connect(model, &QAbstractItemModel::modelReset, model, [&] { ++resets; });
    QObject::connect(&session, &EditSessionAdapter::mediaChanged, &session, [&] { ++media_changes; });
    const auto clips = session.visibleClips(0, 100000);
    for (const auto& value : clips)
        session.selectClip(value.toMap().value(QStringLiteral("id")).toULongLong());
    session.selectClip(0);
    EXPECT_EQ(resets, 0);
    EXPECT_EQ(media_changes, 0);
    EXPECT_TRUE(first.isValid());
}

TEST(EditMediaImport, ProvisionalStreamDescriptionRequiresTheMatchingSourceCapability) {
    EnsureApplication();
    const auto fixture =
        (std::filesystem::path(__FILE__).parent_path() / "../../../libs/engine/tests/fixtures/reordered_h264.mp4")
            .lexically_normal();
    const auto admitted = engine::AcquireMediaReadCapability(fixture);
    ASSERT_TRUE(admitted.succeeded());
    auto metadata = engine::ProbeEditMedia(*admitted.value(), {}, {}, nullptr, false);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    EXPECT_FALSE(metadata.content_identity_valid);
    EXPECT_FALSE(edit::DescribeSourceImport(metadata).descriptor);
    EXPECT_TRUE(edit::DescribeSourceImport(metadata, 0, *admitted.value()).descriptor);
    ++metadata.file_identity.volume_serial;
    EXPECT_FALSE(edit::DescribeSourceImport(metadata, 0, *admitted.value()).descriptor);
    metadata.file_identity = (*admitted.value())->Identity().file_identity;
    ++metadata.file_size;
    EXPECT_FALSE(edit::DescribeSourceImport(metadata, 0, *admitted.value()).descriptor);
}

TEST(EditMediaImport, AFailedNeighborDoesNotDiscardAValidSource) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto fixture =
        (std::filesystem::path(__FILE__).parent_path() / "../../../libs/engine/tests/fixtures/reordered_h264.mp4")
            .lexically_normal();
    ASSERT_TRUE(std::filesystem::exists(fixture));
    EditSessionAdapter session;
    session.importMediaBatch({QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing-a.mkv"))),
                              QUrl::fromLocalFile(QString::fromStdWString(fixture.wstring())),
                              QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing-b.mkv")))},
                             true);
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&session, &EditSessionAdapter::importPendingChanged, &loop, &QEventLoop::quit);
    deadline.start(10000);
    if (session.importPending())
        loop.exec();
    ASSERT_FALSE(session.importPending());
    ASSERT_EQ(session.media().size(), 1);
    const auto id = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    EXPECT_TRUE(session.sourceAccess(id));
    EXPECT_TRUE(session.sourceMetadata(id));
    EXPECT_FALSE(session.workspace().clips().empty());
    EXPECT_TRUE(session.workspaceError().contains(QStringLiteral("missing-a.mkv")));
    EXPECT_TRUE(session.workspaceError().contains(QStringLiteral("missing-b.mkv")));
}

TEST(EditMediaImport, ClosingBeforeAdmissionCannotResurrectTheWorkspace) {
    EnsureApplication();
    const auto fixture =
        (std::filesystem::path(__FILE__).parent_path() / "../../../libs/engine/tests/fixtures/reordered_h264.mp4")
            .lexically_normal();
    EditSessionAdapter session;
    session.importMedia(QUrl::fromLocalFile(QString::fromStdWString(fixture.wstring())), true);
    session.close();
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&session, &EditSessionAdapter::importPendingChanged, &loop, &QEventLoop::quit);
    deadline.start(10000);
    if (session.importPending())
        loop.exec();
    EXPECT_FALSE(session.importPending());
    EXPECT_TRUE(session.workspace().assets().empty());
    EXPECT_TRUE(session.workspace().clips().empty());
}

TEST(EditMediaImport, StalePlacementRequiresAFreshExplicitAction) {
    EnsureApplication();
    const auto fixture =
        (std::filesystem::path(__FILE__).parent_path() / "../../../libs/engine/tests/fixtures/reordered_h264.mp4")
            .lexically_normal();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    const auto before_clips = session.workspace().clips().size();
    const auto before_revision = session.workspace().revision();
    session.importMedia(QUrl::fromLocalFile(QString::fromStdWString(fixture.wstring())), true);
    session.setFixedTimelineEnd(true, 120'000);
    session.setFixedTimelineEnd(false, 120'000);
    ASSERT_GT(session.workspace().revision(), before_revision);
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&session, &EditSessionAdapter::importPendingChanged, &loop, &QEventLoop::quit);
    deadline.start(10000);
    if (session.importPending())
        loop.exec();
    ASSERT_FALSE(session.importPending());
    EXPECT_EQ(session.workspace().clips().size(), before_clips);
    ASSERT_TRUE(session.hasPendingImportedMedia());
    const auto fresh_position = session.workspace().duration();
    ASSERT_TRUE(session.placeImportedMedia(fresh_position, false));
    EXPECT_FALSE(session.hasPendingImportedMedia());
    EXPECT_GT(session.workspace().clips().size(), before_clips);
    EXPECT_FALSE(session.placeImportedMedia(fresh_position, false));
}

TEST(EditMediaImport, MetadataAdmissionDoesNotWaitForAnOccupiedProofScheduler) {
    EnsureApplication();
    const auto fixture =
        (std::filesystem::path(__FILE__).parent_path() / "../../../libs/engine/tests/fixtures/reordered_h264.mp4")
            .lexically_normal();
    QSemaphore started;
    QSemaphore proceed;
    EditSessionAdapter session;
    auto unblock = qScopeGuard([&] { proceed.release(); });
    EditSessionAdapterTestPeer::scheduleSourceVerification(session, [&] {
        started.release();
        proceed.acquire();
    });
    ASSERT_TRUE(started.tryAcquire(1, 1000));
    session.importMedia(QUrl::fromLocalFile(QString::fromStdWString(fixture.wstring())), true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (session.importPending() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    EXPECT_FALSE(session.importPending());
    EXPECT_FALSE(session.workspace().assets().empty());
    EXPECT_FALSE(session.workspace().clips().empty());
}

TEST(EditSelectionSummary, ReportsActualMixedValuesAndEligibleClipCounts) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    ASSERT_EQ(session.media().size(), 1);
    session.appendAsset(session.media().front().toMap().value(QStringLiteral("id")).toULongLong());
    const auto rows = session.visibleClipsUs(0, session.durationUs());
    qulonglong audio = 0;
    qulonglong track = 0;
    for (const auto& value : rows) {
        const auto row = value.toMap();
        if (!row.value(QStringLiteral("video")).toBool()) {
            audio = row.value(QStringLiteral("id")).toULongLong();
            track = row.value(QStringLiteral("trackId")).toULongLong();
            break;
        }
    }
    ASSERT_NE(audio, 0u);
    ASSERT_TRUE(session.setClipGain(audio, 3.0));
    session.selectAllClips();
    auto summary = session.selectedClipSummary();
    EXPECT_EQ(summary.value(QStringLiteral("count")).toInt(), 4);
    EXPECT_EQ(summary.value(QStringLiteral("audioCount")).toInt(), 2);
    EXPECT_EQ(summary.value(QStringLiteral("videoCount")).toInt(), 2);
    EXPECT_TRUE(summary.value(QStringLiteral("gainMixed")).toBool());
    EXPECT_FALSE(summary.contains(QStringLiteral("gainDb")));
    EXPECT_TRUE(summary.value(QStringLiteral("startMixed")).toBool());
    EXPECT_FALSE(summary.contains(QStringLiteral("startUs")));
    EXPECT_FALSE(summary.value(QStringLiteral("inMixed")).toBool());
    EXPECT_TRUE(summary.contains(QStringLiteral("inUs")));
    ASSERT_TRUE(session.setTrackLocked(track, true));
    summary = session.selectedClipSummary();
    EXPECT_EQ(summary.value(QStringLiteral("unlockedCount")).toInt(), 2);
    EXPECT_EQ(summary.value(QStringLiteral("gainEligibleCount")).toInt(), 0);
}

TEST(EditSourceAudioChoice, DraftRemainsUnappliedUntilRealSourceAdmissionAndDoesNotRehash) {
    EnsureApplication();
    const auto path = test::TransportAmbiguousFixture();
    ASSERT_FALSE(path.isEmpty());
    EditSessionAdapter session;
    session.importMedia(QUrl::fromLocalFile(path), true);
    const auto wait_until = [&](const auto& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return done();
    };
    ASSERT_TRUE(wait_until([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    ASSERT_TRUE(wait_until([&] { return bool(session.sourceProof(asset)); }));
    ASSERT_TRUE(session.workspace().asset(asset)->mix_decision_required);
    const auto revision = session.workspace().revision();
    const auto hashes = session.sourceAccess(asset)->Measurements().hash_starts;
    ASSERT_TRUE(session.beginSourceAudioChoice(asset));
    const auto tracks = session.sourceAudioChoice().value(QStringLiteral("tracks")).toList();
    ASSERT_EQ(tracks.size(), 2);
    const auto first = tracks.front().toMap();
    EXPECT_FALSE(first.value(QStringLiteral("name")).toString().isEmpty());
    EXPECT_FALSE(session.sourceAudioChoice().value(QStringLiteral("overlapRequired")).toBool());
    session.setSourceAudioTrack(first.value(QStringLiteral("id")).toString(), true);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_TRUE(session.workspace().asset(asset)->mix_decision_required);
    EXPECT_FALSE(session.sourceAudioChoice().value(QStringLiteral("overlapRequired")).toBool());
    const auto second = tracks.back().toMap().value(QStringLiteral("id")).toString();
    session.setSourceAudioTrack(second, true);
    EXPECT_TRUE(session.sourceAudioChoice().value(QStringLiteral("overlapRequired")).toBool());
    EXPECT_FALSE(session.sourceAudioChoice().value(QStringLiteral("canApply")).toBool());
    session.setSourceAudioTrack(second, false);
    EXPECT_TRUE(session.sourceAudioChoice().value(QStringLiteral("canApply")).toBool());
    const auto audio_track = std::find_if(session.workspace().tracks().begin(), session.workspace().tracks().end(),
                                          [](const auto& track) { return track.type == edit::TrackType::Audio; });
    ASSERT_NE(audio_track, session.workspace().tracks().end());
    const auto audio_track_id = audio_track->id;
    ASSERT_TRUE(session.setTrackLocked(audio_track_id, true));
    EXPECT_FALSE(session.sourceAudioChoice().value(QStringLiteral("canApply")).toBool());
    EXPECT_FALSE(session.applySourceAudioChoice());
    ASSERT_TRUE(session.setTrackLocked(audio_track_id, false));
    ASSERT_TRUE(session.applySourceAudioChoice());
    ASSERT_TRUE(wait_until([&] { return !session.sourceAudioChoice().value(QStringLiteral("busy")).toBool(); }));
    ASSERT_FALSE(session.workspace().asset(asset)->mix_decision_required);
    ASSERT_TRUE(session.workspace().asset(asset)->input_mix_selection);
    EXPECT_EQ(session.workspace().asset(asset)->input_mix_selection->selected_stream_identities.size(), 1u);
    EXPECT_EQ(session.sourceAccess(asset)->Measurements().hash_starts, hashes);
}

TEST(EditMediaLifecycle, CloseReleasesAPausedVerificationLeaseWithoutWaitingForAdmission) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("close-pending.mkv"));
    ASSERT_TRUE(QFile::copy(test::TransportAmbiguousFixture(), path));
    EditSessionAdapter session;
    session.setBackgroundAnalysisSuspended(true);
    session.importMedia(QUrl::fromLocalFile(path), true);
    const auto wait_until = [&](const auto& done, int timeout_ms) {
        QElapsedTimer deadline;
        deadline.start();
        while (!done() && deadline.elapsed() < timeout_ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        return done();
    };
    ASSERT_TRUE(wait_until([&] { return !session.importPending(); }, 5000));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    // Suspension defers optional verification before it starts (see
    // EditRecordedDefault), so the publication holds the lease without hashing.
    ASSERT_NE(session.sourceAccess(asset), nullptr);
    EXPECT_FALSE(wait_until([&] { return session.sourceAccess(asset)->Measurements().hash_starts != 0; }, 200));
    EXPECT_EQ(session.sourceAccess(asset)->Measurements().hash_completions, 0u);
    const std::weak_ptr<const engine::MediaReadCapability> lease = session.sourceAccess(asset);
    QElapsedTimer close_time;
    close_time.start();
    session.close();
    EXPECT_LT(close_time.elapsed(), 100);
    ASSERT_TRUE(wait_until([&] { return lease.expired(); }, 1000));
    EXPECT_TRUE(session.workspace().assets().empty());
    EXPECT_FALSE(session.sourceAudioChoice().value(QStringLiteral("active")).toBool());
}

TEST(EditFolders, TenThousandMixedEntriesRemainLazyCancelableAndStableAcrossRefreshAndDistantImports) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto nested = QStringLiteral("closed-nested");
    ASSERT_TRUE(QDir(directory.path()).mkdir(nested));
    QFile nested_file(directory.filePath(nested + QStringLiteral("/not-enumerated.mkv")));
    ASSERT_TRUE(nested_file.open(QIODevice::WriteOnly));
    nested_file.close();
    QList<QUrl> distant;
    for (int index = 0; index < 10000; ++index) {
        const auto name = QStringLiteral("entry-%1.%2")
                              .arg(index, 5, 10, QLatin1Char('0'))
                              .arg(index % 2 ? QStringLiteral("txt") : QStringLiteral("mkv"));
        const auto path = directory.filePath(name);
        if (index == 0 || index == 5000 || index == 9998) {
            ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("red.mkv")), path));
            distant.push_back(QUrl::fromLocalFile(path));
        } else {
            QFile file(path);
            ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        }
    }
    QFile long_name(
        directory.filePath(QStringLiteral("long-") + QString(160, QLatin1Char('x')) + QString::fromUtf8("-äöü.txt")));
    ASSERT_TRUE(long_name.open(QIODevice::WriteOnly));
    long_name.close();
    EditFolderModel model;
    model.addRoot(QUrl::fromLocalFile(directory.path()));
    ASSERT_EQ(model.rowCount(), 1);
    const auto root_id = model.data(model.index(0), EditTimelineClipModel::RowRole)
                             .toMap()
                             .value(QStringLiteral("entryId"))
                             .toULongLong();
    int resets = 0;
    int largest_batch = 0;
    int heartbeat_count = 0;
    QElapsedTimer elapsed;
    QElapsedTimer heartbeat_time;
    qint64 largest_heartbeat_ms = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &model, [&] {
        largest_heartbeat_ms = std::max(largest_heartbeat_ms, heartbeat_time.restart());
        ++heartbeat_count;
    });
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model, [&](const QModelIndex&, int first, int last) {
        largest_batch = std::max(largest_batch, last - first + 1);
    });
    const auto wait_until = [&](const auto& done, int timeout_ms = 20000) {
        QElapsedTimer deadline;
        deadline.start();
        while (!done() && deadline.elapsed() < timeout_ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        return done();
    };
    elapsed.start();
    heartbeat_time.start();
    heartbeat.start(1);
    model.toggle(root_id);
    ASSERT_TRUE(wait_until([&] { return model.rowCount() > 1; }));
    const auto first_useful_ms = elapsed.elapsed();
    const auto first_materialized = model.rowCount() - 1;
    EXPECT_LE(first_materialized, 200);
    EXPECT_LT(first_materialized, 10002);
    model.toggle(root_id);
    ASSERT_EQ(model.rowCount(), 1);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    EXPECT_EQ(model.rowCount(), 1);
    model.toggle(root_id);
    ASSERT_TRUE(wait_until([&] {
        return model.rowCount() == 10003 && !model.data(model.index(0), EditTimelineClipModel::RowRole)
                                                 .toMap()
                                                 .value(QStringLiteral("loading"))
                                                 .toBool();
    }));
    QList<QPersistentModelIndex> saved;
    const int preselected_rows[] = {2, 5002, 10000};
    for (qsizetype offset = 0; offset < distant.size(); ++offset) {
        const auto index = model.index(preselected_rows[offset]);
        ASSERT_EQ(model.data(index, EditTimelineClipModel::RowRole).toMap().value(QStringLiteral("url")).toUrl(),
                  distant[offset]);
        saved.push_back(QPersistentModelIndex(index));
    }
    ASSERT_EQ(saved.size(), 3);
    QList<qulonglong> ids;
    for (const auto& index : saved)
        ids.push_back(
            model.data(index, EditTimelineClipModel::RowRole).toMap().value(QStringLiteral("entryId")).toULongLong());
    model.refresh(root_id);
    ASSERT_TRUE(wait_until([&] {
        return !model.data(model.index(0), EditTimelineClipModel::RowRole)
                    .toMap()
                    .value(QStringLiteral("loading"))
                    .toBool();
    }));
    for (qsizetype index = 0; index < saved.size(); ++index) {
        EXPECT_TRUE(saved[index].isValid());
        EXPECT_EQ(model.data(saved[index], EditTimelineClipModel::RowRole)
                      .toMap()
                      .value(QStringLiteral("entryId"))
                      .toULongLong(),
                  ids[index]);
    }
    heartbeat.stop();
    EXPECT_GT(heartbeat_count, 10);
    EXPECT_LE(largest_batch, 200);
    EXPECT_EQ(resets, 0);
    EditSessionAdapter session;
    session.importMediaBatch(distant, true);
    ASSERT_TRUE(wait_until([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 3);
    for (qsizetype index = 0; index < distant.size(); ++index)
        EXPECT_EQ(session.media()[index].toMap().value(QStringLiteral("path")).toString(),
                  distant[index].toLocalFile());
    qInfo("DIR10K entries=10003 firstUsefulMs=%lld materializedAtFirstInteraction=%d largestBatch=%d heartbeatCount=%d "
          "largestHeartbeatMs=%lld enumeratedDirectory=rootOnly distantImports=3",
          first_useful_ms, first_materialized, largest_batch, heartbeat_count, largest_heartbeat_ms);
    ::testing::Test::RecordProperty("treeEntries", 10003);
    ::testing::Test::RecordProperty("rootDirectoryEntries", 10002);
    ::testing::Test::RecordProperty("firstUsefulMs", std::to_string(first_useful_ms));
    ::testing::Test::RecordProperty("materializedAtFirstInteraction", first_materialized);
    ::testing::Test::RecordProperty("largestBatch", largest_batch);
    ::testing::Test::RecordProperty("heartbeatCount", heartbeat_count);
    ::testing::Test::RecordProperty("largestHeartbeatMs", std::to_string(largest_heartbeat_ms));
    ::testing::Test::RecordProperty("enumeratedDirectories", "rootOnly");
    ::testing::Test::RecordProperty(
        "publicationStrategy", "full immediate metadata scan, incremental model rows, no recursive or codec probe");
    ::testing::Test::RecordProperty("distantImports", 3);
    ::testing::Test::RecordProperty("preselectedRows", "2,5002,10000");
}

TEST(EditFolders, CyclicAndBrokenDirectoryLinksAreReportedWithoutFollowingThem) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto root = std::filesystem::path(directory.path().toStdWString());
    std::error_code error;
    std::filesystem::create_directory_symlink(root, root / "cycle", error);
    if (error)
        GTEST_SKIP() << "Directory symlink creation is unavailable on this host: " << error.message();
    std::filesystem::create_directory_symlink(root / "missing-target", root / "broken", error);
    ASSERT_FALSE(error) << error.message();
    EditFolderModel model;
    model.addRoot(QUrl::fromLocalFile(directory.path()));
    const auto id = model.data(model.index(0), EditTimelineClipModel::RowRole)
                        .toMap()
                        .value(QStringLiteral("entryId"))
                        .toULongLong();
    model.toggle(id);
    QElapsedTimer deadline;
    deadline.start();
    while (model.rowCount() < 3 && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    ASSERT_EQ(model.rowCount(), 3);
    qulonglong cycle_id = 0;
    QPersistentModelIndex cycle_index;
    for (int row = 1; row < model.rowCount(); ++row) {
        const auto entry = model.data(model.index(row), EditTimelineClipModel::RowRole).toMap();
        EXPECT_FALSE(entry.value(QStringLiteral("error")).toString().isEmpty());
        EXPECT_FALSE(entry.value(QStringLiteral("mediaCandidate")).toBool());
        if (entry.value(QStringLiteral("name")).toString() == QStringLiteral("cycle")) {
            cycle_id = entry.value(QStringLiteral("entryId")).toULongLong();
            cycle_index = model.index(row);
        }
    }
    ASSERT_NE(cycle_id, 0u);
    model.toggle(cycle_id);
    deadline.restart();
    while (model.data(cycle_index, EditTimelineClipModel::RowRole).toMap().value(QStringLiteral("loading")).toBool() &&
           deadline.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    EXPECT_EQ(model.rowCount(), 3);
    EXPECT_FALSE(model.data(cycle_index, EditTimelineClipModel::RowRole)
                     .toMap()
                     .value(QStringLiteral("error"))
                     .toString()
                     .isEmpty());
}

TEST(EditMediaImport, ClosingAProjectOnlySessionReleasesItsAssetsAndSources) {
    EnsureApplication();
    EditSessionAdapter session;
    engine::EditMediaMetadata metadata;
    metadata.content_identity_valid = true;
    metadata.content_sha256.fill(29);
    metadata.duration_us = 2'000'000;
    metadata.has_audio = true;
    engine::EditSourceStream stream;
    stream.kind = engine::TimelineStreamKind::Audio;
    stream.container_index = 0;
    stream.codec.channels = 2;
    metadata.streams.push_back(stream);
    const auto id = session.addProbedMediaForHarness(QStringLiteral("project-only.mkv"), metadata, false);
    ASSERT_NE(id, 0u);
    ASSERT_FALSE(session.workspace().assets().empty());
    session.close();
    EXPECT_TRUE(session.workspace().assets().empty());
    EXPECT_TRUE(session.media().empty());
    EXPECT_FALSE(session.hasPendingImportedMedia());
}

TEST(EditFolders, ExpansionIsLazyBatchedAndRefreshPreservesExistingRows) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir(directory.path()).mkdir(QStringLiteral("Nested")));
    for (int i = 0; i < 1000; ++i) {
        QFile file(directory.filePath(QStringLiteral("clip-%1.mp4").arg(i, 4, 10, QLatin1Char('0'))));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
    EditFolderModel model;
    model.addRoot(QUrl::fromLocalFile(directory.path()));
    ASSERT_EQ(model.rowCount(), 1);
    const auto root = model.data(model.index(0), EditTimelineClipModel::RowRole).toMap();
    const auto id = root.value(QStringLiteral("entryId")).toULongLong();
    int resets = 0;
    int largest_batch = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model, [&](const QModelIndex&, int first, int last) {
        largest_batch = std::max(largest_batch, last - first + 1);
    });
    model.toggle(id);
    const auto wait_until = [&](const auto& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        return done();
    };
    ASSERT_TRUE(wait_until([&] {
        return model.rowCount() == 1002 && !model.data(model.index(0), EditTimelineClipModel::RowRole)
                                                .toMap()
                                                .value(QStringLiteral("loading"))
                                                .toBool();
    }));
    EXPECT_LE(largest_batch, 200);
    EXPECT_EQ(resets, 0);
    const QPersistentModelIndex saved(model.index(1));
    const auto saved_row = model.data(saved, EditTimelineClipModel::RowRole).toMap();
    QFile added(directory.filePath(QStringLiteral("z-last.wav")));
    ASSERT_TRUE(added.open(QIODevice::WriteOnly));
    added.close();
    model.refresh(id);
    ASSERT_TRUE(wait_until([&] { return model.rowCount() == 1003; }));
    ASSERT_TRUE(saved.isValid());
    EXPECT_EQ(model.data(saved, EditTimelineClipModel::RowRole).toMap().value(QStringLiteral("entryId")),
              saved_row.value(QStringLiteral("entryId")));
    model.toggle(id);
    EXPECT_EQ(model.rowCount(), 1);
}

TEST(EditMediaDrop, DirectoryChoiceUsesOneOrderedCandidateBatchAndCancelHasNoEffect) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    for (const auto& name : {QStringLiteral("b.wav"), QStringLiteral("A.mp4"), QStringLiteral("notes.txt")}) {
        QFile file(directory.filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
    EditSessionAdapter session;
    qulonglong request = 0;
    QVariantList roots;
    QVariantList candidates;
    qint64 at = -1;
    QString reason;
    QObject::connect(&session, &EditSessionAdapter::directoryDropRequested, &session,
                     [&](qulonglong id, const QVariantList& directories, const QVariantList& files, qint64 time,
                         const QString& error) {
                         request = id;
                         roots = directories;
                         candidates = files;
                         at = time;
                         reason = error;
                     });
    session.routeMediaDrop({QUrl::fromLocalFile(directory.path())}, true, 123'456);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!request && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    ASSERT_NE(request, 0u);
    ASSERT_EQ(roots.size(), 1);
    ASSERT_EQ(candidates.size(), 2);
    EXPECT_TRUE(reason.isEmpty());
    EXPECT_EQ(at, 123'456);
    EXPECT_EQ(QFileInfo(candidates[0].toUrl().toLocalFile()).fileName(), QStringLiteral("A.mp4"));
    EXPECT_EQ(QFileInfo(candidates[1].toUrl().toLocalFile()).fileName(), QStringLiteral("b.wav"));
    const auto prior_roots = session.folderModel()->rowCount();
    session.cancelMediaDrop(request);
    session.resolveMediaDrop(request, false);
    EXPECT_TRUE(session.workspace().assets().empty());
    EXPECT_TRUE(session.workspace().clips().empty());
    EXPECT_EQ(session.folderModel()->rowCount(), prior_roots);
}

TEST(EditFolders, DirectoryInsertionCountsOnlyOrderedImmediateRegularMediaCandidates) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir(directory.path()).mkdir(QStringLiteral("nested")));
    for (const auto& name : {QStringLiteral("z.WAV"), QStringLiteral("B.mp4"), QStringLiteral("a.mkv"),
                             QStringLiteral("notes.txt"), QStringLiteral("nested/hidden.mp4")}) {
        QFile file(directory.filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
    EditFolderModel model;
    QVariantList candidates;
    QString reason;
    bool finished = false;
    QObject::connect(&model, &EditFolderModel::candidatesReady, &model,
                     [&](const QUrl&, const QVariantList& files, const QString& error) {
                         candidates = files;
                         reason = error;
                         finished = true;
                     });
    model.inspectDirectory(QUrl::fromLocalFile(directory.path()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    ASSERT_TRUE(finished);
    EXPECT_TRUE(reason.isEmpty()) << reason.toStdString();
    ASSERT_EQ(candidates.size(), 3);
    EXPECT_EQ(QFileInfo(candidates[0].toUrl().toLocalFile()).fileName(), QStringLiteral("a.mkv"));
    EXPECT_EQ(QFileInfo(candidates[1].toUrl().toLocalFile()).fileName(), QStringLiteral("B.mp4"));
    EXPECT_EQ(QFileInfo(candidates[2].toUrl().toLocalFile()).fileName(), QStringLiteral("z.WAV"));
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(EditTimelineClipModel, ViewportFiltersTracksBeforePublishingRowsAndKeepsSurvivingIds) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    ASSERT_EQ(session.workspace().tracks().size(), 2u);
    const auto first = session.workspace().tracks()[0].id;
    const auto second = session.workspace().tracks()[1].id;
    EditTimelineClipModel model;
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&] { ++resets; });
    model.syncViewportUs(&session, 0, 100'000'000, {QVariant::fromValue<qulonglong>(first)});
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.data(model.index(0), EditTimelineClipModel::RowRole)
                  .toMap()
                  .value(QStringLiteral("trackId"))
                  .toULongLong(),
              first);
    const QPersistentModelIndex saved(model.index(0));
    const auto saved_id = model.data(saved, EditTimelineClipModel::RowRole).toMap().value(QStringLiteral("id"));
    model.syncViewportUs(&session, 0, 100'000'000,
                         {QVariant::fromValue<qulonglong>(first), QVariant::fromValue<qulonglong>(second)});
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_TRUE(saved.isValid());
    EXPECT_EQ(model.data(saved, EditTimelineClipModel::RowRole).toMap().value(QStringLiteral("id")), saved_id);
    model.syncViewportUs(&session, 0, 100'000'000, {});
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(resets, 0);
}

TEST(EditTimelineClipModel, AnExplicitPointerCaptureSurvivesBothViewportAxesWithoutRetainingOtherClips) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    ASSERT_EQ(session.workspace().clips().size(), 2u);
    const auto captured = session.workspace().clips().front().id;
    const auto revision = session.workspace().revision();
    EditTimelineClipModel model;
    model.syncViewportUs(&session, 200'000'000, 201'000'000, {}, {QVariant::fromValue<qulonglong>(captured)});
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(
        model.data(model.index(0), EditTimelineClipModel::RowRole).toMap().value(QStringLiteral("id")).toULongLong(),
        captured);
    model.syncViewportUs(&session, 200'000'000, 201'000'000, {}, {});
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(session.workspace().revision(), revision);
}

TEST(EditMediaLifecycle, CancelingANewBatchKeepsThePlacedSourceVerificationAlive) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto first_path = directory.filePath(QStringLiteral("placed.mkv"));
    const auto second_path = directory.filePath(QStringLiteral("cancelled.mkv"));
    ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("red.mkv")), first_path));
    ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("blue.mkv")), second_path));
    EditSessionAdapter session;
    session.setBackgroundAnalysisSuspended(true);
    session.importMedia(QUrl::fromLocalFile(first_path), true);
    const auto wait_until = [&](const auto& done) {
        QElapsedTimer deadline;
        deadline.start();
        while (!done() && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        return done();
    };
    ASSERT_TRUE(wait_until([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    // Suspension defers the placed source's verification before it hashes.
    EXPECT_EQ(session.sourceAccess(asset)->Measurements().hash_starts, 0u);
    const auto revision = session.workspace().revision();
    session.importMedia(QUrl::fromLocalFile(second_path), true);
    session.cancelPendingImports();
    EXPECT_FALSE(session.sourceVerificationFailed(asset));
    EXPECT_TRUE(session.sourceVerificationReason(asset).isEmpty());
    session.setBackgroundAnalysisSuspended(false);
    ASSERT_TRUE(wait_until([&] { return bool(session.sourceProof(asset)); }));
    ASSERT_TRUE(wait_until([&] { return !session.importPending(); }));
    EXPECT_EQ(session.media().size(), 1);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(session.sourceAccess(asset)->Measurements().hash_starts, 1u);
    EXPECT_EQ(session.sourceAccess(asset)->Measurements().hash_cancellations, 0u);
}

TEST(EditTrackLayout, VisibleTrackIdsUseHalfOpenBaseRowsAndIgnoreCandidateGeometry) {
    EnsureApplication();
    EditTrackLayoutModel layout;
    const QVariantMap first{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(11)},
                            {QStringLiteral("video"), true}};
    const QVariantMap second{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(22)},
                             {QStringLiteral("video"), false}};
    layout.setTracks({first, second});
    EXPECT_EQ(layout.trackIdsIntersecting(0, 64), QVariantList{QVariant::fromValue<qulonglong>(11)});
    EXPECT_EQ(layout.trackIdsIntersecting(64, 108), QVariantList{QVariant::fromValue<qulonglong>(22)});
    EXPECT_EQ(layout.trackIdsIntersecting(63, 65),
              (QVariantList{QVariant::fromValue<qulonglong>(11), QVariant::fromValue<qulonglong>(22)}));
    EXPECT_TRUE(layout.trackIdsIntersecting(64, 64).empty());
    EXPECT_TRUE(layout.trackIdsIntersecting(108, 200).empty());
    EXPECT_TRUE(layout.trackIdsIntersecting(std::numeric_limits<qreal>::quiet_NaN(), 100).empty());
    layout.setPreviewTracks({second, first});
    EXPECT_EQ(layout.trackIdsIntersecting(0, 64), QVariantList{QVariant::fromValue<qulonglong>(11)});
}

TEST(EditSelectionSummary, OfflineAudioIdsRemainCompleteWhileLocksControlGainEligibility) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto context = MakeContext(5);
    context.output_path = directory.filePath(QStringLiteral("missing.mkv"));
    EditSessionAdapter session;
    session.setEditContext(context);
    ASSERT_EQ(session.workspace().assets().size(), 1U);
    ASSERT_EQ(session.workspace().assets().front().state, edit::AssetState::Missing);
    session.selectAllClips();
    const auto summary = session.selectedClipSummary();
    ASSERT_EQ(summary.value(QStringLiteral("audioCount")).toInt(), 1);
    const auto ids = summary.value(QStringLiteral("audioIds")).toList();
    ASSERT_EQ(ids.size(), 1);
    const auto audio = ids.front().toULongLong();
    const auto clip = std::find_if(session.workspace().clips().begin(), session.workspace().clips().end(),
                                   [audio](const auto& item) { return item.id == audio; });
    ASSERT_NE(clip, session.workspace().clips().end());
    const auto track = clip->track;
    EXPECT_EQ(summary.value(QStringLiteral("gainEligibleCount")).toInt(), 1);
    ASSERT_TRUE(session.setClipGain(audio, 2.0));
    ASSERT_TRUE(session.setTrackLocked(track, true));
    const auto locked = session.selectedClipSummary();
    EXPECT_EQ(locked.value(QStringLiteral("audioIds")).toList(), ids);
    EXPECT_EQ(locked.value(QStringLiteral("gainEligibleCount")).toInt(), 0);
}

TEST(EditSourceReplacementAdapter, ReviewedAdoptionChangesOneVersionAndUndoRestoresHeldBytes) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto old_path = directory.filePath(QStringLiteral("original.mkv"));
    const auto new_path = directory.filePath(QStringLiteral("replacement.mkv"));
    ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("red.mkv")), old_path));
    ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("blue.mkv")), new_path));
    EditSessionAdapter session;
    const auto wait_until = [&](const auto& done) {
        QElapsedTimer deadline;
        deadline.start();
        while (!done() && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        return done();
    };
    session.importMedia(QUrl::fromLocalFile(old_path), true);
    ASSERT_TRUE(wait_until([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    ASSERT_TRUE(wait_until([&] { return bool(session.sourceProof(asset)); }));
    session.appendAsset(asset);
    const auto before = session.workspace().clips();
    const auto old_capability = session.sourceAccess(asset);
    const auto old_proof = session.sourceProof(asset);
    const auto old_descriptor = session.workspace().asset(asset)->source_descriptor;
    const auto revision = session.workspace().revision();
    session.beginSourceReplacement(asset, QUrl::fromLocalFile(new_path), true);
    ASSERT_TRUE(session.sourceReplacement().value(QStringLiteral("active")).toBool());
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(session.sourceAccess(asset), old_capability);
    ASSERT_TRUE(wait_until([&] { return session.sourceReplacement().value(QStringLiteral("canApply")).toBool(); }))
        << session.sourceReplacement().value(QStringLiteral("reason")).toString().toStdString();
    EXPECT_EQ(session.sourceReplacement().value(QStringLiteral("affectedClips")).toList().size(), before.size());
    ASSERT_TRUE(session.applySourceReplacement());
    ASSERT_TRUE(wait_until([&] { return session.workspace().asset(asset)->source_version != 0; }));
    EXPECT_EQ(session.workspace().revision(), revision + 1);
    EXPECT_EQ(session.workspace().asset(asset)->path, new_path.toStdWString());
    const auto new_capability = session.sourceAccess(asset);
    const auto new_proof = session.sourceProof(asset);
    ASSERT_NE(new_capability, old_capability);
    ASSERT_NE(new_proof, nullptr);
    EXPECT_NE(new_proof->Identity().sha256, old_proof->Identity().sha256);
    for (const auto& old_clip : before) {
        const auto* current = session.workspace().clip(old_clip.id);
        ASSERT_NE(current, nullptr);
        EXPECT_EQ(current->start, old_clip.start);
        EXPECT_EQ(current->source_in, old_clip.source_in);
        EXPECT_EQ(current->source_out, old_clip.source_out);
        EXPECT_EQ(current->group, old_clip.group);
    }
    session.undo();
    EXPECT_EQ(session.workspace().asset(asset)->path, old_path.toStdWString());
    EXPECT_EQ(session.workspace().asset(asset)->source_descriptor, old_descriptor);
    EXPECT_EQ(session.sourceAccess(asset), old_capability);
    EXPECT_EQ(session.sourceProof(asset), old_proof);
    session.redo();
    EXPECT_EQ(session.workspace().asset(asset)->path, new_path.toStdWString());
    EXPECT_EQ(session.sourceAccess(asset), new_capability);
    EXPECT_EQ(session.sourceProof(asset), new_proof);
}

TEST(EditSourceReplacementAdapter, WrongIdentityAndWrongLayoutStayDraftOnlyAndCloseRevokesVerification) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    EditSessionAdapter session;
    const auto wait_until = [&](const auto& done) {
        QElapsedTimer deadline;
        deadline.start();
        while (!done() && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        return done();
    };
    session.importMedia(QUrl::fromLocalFile(QDir(fixtures).filePath(QStringLiteral("red.mkv"))), true);
    ASSERT_TRUE(wait_until([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    ASSERT_TRUE(wait_until([&] { return bool(session.sourceProof(asset)); }));
    const auto before = session.workspace().snapshot();
    const auto old_capability = session.sourceAccess(asset);
    for (const auto& candidate :
         {std::pair{QStringLiteral("blue.mkv"), false}, std::pair{QStringLiteral("transport-audio-only.mka"), true}}) {
        session.beginSourceReplacement(asset, QUrl::fromLocalFile(QDir(fixtures).filePath(candidate.first)),
                                       candidate.second);
        ASSERT_TRUE(session.sourceReplacement().value(QStringLiteral("active")).toBool());
        ASSERT_TRUE(wait_until([&] { return !session.sourceReplacement().value(QStringLiteral("busy")).toBool(); }));
        EXPECT_FALSE(session.sourceReplacement().value(QStringLiteral("canApply")).toBool());
        EXPECT_FALSE(session.sourceReplacement().value(QStringLiteral("reason")).toString().isEmpty());
        EXPECT_FALSE(session.applySourceReplacement());
        EXPECT_EQ(session.workspace().revision(), before->revision);
        EXPECT_EQ(session.workspace().clips(), before->clips);
        EXPECT_EQ(session.sourceAccess(asset), old_capability);
        session.cancelSourceReplacement();
        EXPECT_FALSE(session.sourceReplacement().value(QStringLiteral("active")).toBool());
    }
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto paused_path = directory.filePath(QStringLiteral("paused-replacement.mkv"));
    ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("blue.mkv")), paused_path));
    session.setBackgroundAnalysisSuspended(true);
    session.beginSourceReplacement(asset, QUrl::fromLocalFile(paused_path), true);
    ASSERT_TRUE(session.sourceReplacement().value(QStringLiteral("active")).toBool());
    ASSERT_TRUE(wait_until([&] {
        return session.sourceReplacement().value(QStringLiteral("stage")).toString() == QStringLiteral("verification");
    }));
    session.close();
    EXPECT_FALSE(session.sourceReplacement().value(QStringLiteral("active")).toBool());
    session.setBackgroundAnalysisSuspended(false);
    ASSERT_TRUE(wait_until([&] { return session.workspace().assets().empty() && !session.sourceAccess(asset); }));
    EXPECT_TRUE(session.workspace().clips().empty());
}

bool WaitForMediaCondition(const std::function<bool()>& done) {
    QElapsedTimer deadline;
    deadline.start();
    while (!done() && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return done();
}

TEST(EditSourceReplacementAdapter, IdenticalBytesRelinkWithoutAdoptionAndSpecialPathsStayExact) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto original = QDir(fixtures).filePath(QStringLiteral("red.mkv"));
    const auto replacement = directory.filePath(QString::fromUtf8("same # % ü.mkv"));
    ASSERT_TRUE(QFile::copy(original, replacement));
    EditSessionAdapter session;
    session.importMedia(QUrl::fromLocalFile(original), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
    const auto old_capability = session.sourceAccess(asset);
    const auto old_digest = session.sourceProof(asset)->Identity().sha256;
    const auto revision = session.workspace().revision();
    session.beginSourceReplacement(asset, QUrl::fromLocalFile(replacement));
    ASSERT_TRUE(WaitForMediaCondition([&] {
        return session.sourceReplacement().value(QStringLiteral("canApply")).toBool();
    })) << session.sourceReplacement().value(QStringLiteral("reason")).toString().toStdString();
    const auto review = session.sourceReplacement();
    EXPECT_FALSE(review.value(QStringLiteral("adoptChangedBytes")).toBool());
    EXPECT_EQ(review.value(QStringLiteral("url")).toUrl().toLocalFile(), replacement);
    EXPECT_EQ(session.workspace().revision(), revision);
    ASSERT_TRUE(session.applySourceReplacement());
    ASSERT_TRUE(
        WaitForMediaCondition([&] { return !session.sourceReplacement().value(QStringLiteral("active")).toBool(); }));
    EXPECT_EQ(session.workspace().revision(), revision + 1);
    EXPECT_EQ(session.workspace().asset(asset)->path, replacement.toStdWString());
    EXPECT_NE(session.sourceAccess(asset), old_capability);
    EXPECT_EQ(session.sourceProof(asset)->Identity().sha256, old_digest);
    EXPECT_EQ(session.sourceAccess(asset)->Measurements().hash_starts, 1U);
}

TEST(EditSourceReplacementAdapter, AuthoredChangesAndLocksInvalidateTheCompleteReviewedCandidate) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    EditSessionAdapter session;
    session.importMedia(QUrl::fromLocalFile(QDir(fixtures).filePath(QStringLiteral("red.mkv"))), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
    const auto original_capability = session.sourceAccess(asset);
    const auto candidate = QUrl::fromLocalFile(QDir(fixtures).filePath(QStringLiteral("blue.mkv")));
    session.beginSourceReplacement(asset, candidate, true);
    ASSERT_TRUE(
        WaitForMediaCondition([&] { return session.sourceReplacement().value(QStringLiteral("canApply")).toBool(); }));
    session.appendAsset(asset);
    const auto changed_revision = session.workspace().revision();
    EXPECT_FALSE(session.sourceReplacement().value(QStringLiteral("canApply")).toBool());
    EXPECT_FALSE(session.applySourceReplacement());
    EXPECT_EQ(session.workspace().revision(), changed_revision);
    EXPECT_EQ(session.sourceAccess(asset), original_capability);
    session.beginSourceReplacement(asset, candidate, true);
    ASSERT_TRUE(
        WaitForMediaCondition([&] { return session.sourceReplacement().value(QStringLiteral("canApply")).toBool(); }));
    const auto track = session.workspace().clips().front().track;
    ASSERT_TRUE(session.setTrackLocked(track, true));
    const auto locked_revision = session.workspace().revision();
    EXPECT_FALSE(session.applySourceReplacement());
    session.beginSourceReplacement(asset, candidate, true);
    ASSERT_TRUE(
        WaitForMediaCondition([&] { return !session.sourceReplacement().value(QStringLiteral("busy")).toBool(); }));
    EXPECT_FALSE(session.sourceReplacement().value(QStringLiteral("canApply")).toBool());
    EXPECT_FALSE(session.applySourceReplacement());
    EXPECT_EQ(session.workspace().revision(), locked_revision);
    EXPECT_EQ(session.workspace().asset(asset)->source_version, 0U);
    EXPECT_EQ(session.sourceAccess(asset), original_capability);
}

TEST(EditSourceReplacementAdapter, CancellingReviewFromGestureNotificationCannotCommitOrDereferenceItsDraft) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    EditSessionAdapter session;
    session.importMedia(QUrl::fromLocalFile(QDir(fixtures).filePath(QStringLiteral("red.mkv"))), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
    session.beginSourceReplacement(asset, QUrl::fromLocalFile(QDir(fixtures).filePath(QStringLiteral("blue.mkv"))),
                                   true);
    ASSERT_TRUE(
        WaitForMediaCondition([&] { return session.sourceReplacement().value(QStringLiteral("canApply")).toBool(); }));
    ASSERT_FALSE(session.workspace().clips().empty());
    const auto before = session.workspace().snapshot();
    const auto capability = session.sourceAccess(asset);
    session.beginClipDrag(session.workspace().clips().front().id);
    ASSERT_TRUE(session.dragActive());
    QObject::connect(&session, &EditSessionAdapter::dragChanged, &session, [&] {
        if (!session.dragActive())
            session.cancelSourceReplacement();
    });
    ASSERT_TRUE(session.applySourceReplacement());
    ASSERT_TRUE(
        WaitForMediaCondition([&] { return !session.sourceReplacement().value(QStringLiteral("active")).toBool(); }));
    EXPECT_EQ(session.workspace().revision(), before->revision);
    EXPECT_EQ(session.workspace().clips(), before->clips);
    EXPECT_EQ(session.sourceAccess(asset), capability);
}
TEST(EditPlaybackSourceIntegrity, SamePathAndStateCannotHideAnotherAuthoredSourceVersion) {
    edit::Asset source;
    source.path = L"identical-path.mkv";
    source.duration = 1'000'000;
    source.state = edit::AssetState::Available;
    source.source_version = 1;
    edit::Workspace before;
    edit::Workspace after;
    const auto first = before.addAsset(source);
    source.source_version = 2;
    ASSERT_EQ(after.addAsset(source), first);
    const EditPlaybackSnapshot old_snapshot(before);
    const EditPlaybackSnapshot new_snapshot(after);
    EXPECT_FALSE(old_snapshot.retainsMedia(new_snapshot));
    EXPECT_TRUE(old_snapshot.retainsMedia(EditPlaybackSnapshot(before)));
}

TEST(EditPlaybackSourceIntegrity, MonitoringAuthorityCannotHideBehindAnUnchangedSelectionRevision) {
    engine::MonitorSelectionSnapshot selection;
    selection.media_id.bytes[0] = 1;
    selection.revision = {7};
    engine::AudioTrackId track;
    track.bytes[0] = 2;
    selection.tracks.push_back({track});
    auto binding = std::make_shared<engine::UnverifiedAudioAssetBinding>();
    binding->file_size = 100;
    binding->sha256[0] = 3;
    selection.unverified_asset_binding = binding;
    edit::Asset source;
    source.path = L"monitoring-source.mkv";
    source.duration = 1'000'000;
    source.state = edit::AssetState::Available;
    source.monitor_selection = selection;
    edit::Workspace workspace;
    const auto asset = workspace.addAsset(source);
    const EditPlaybackSnapshot snapshot(workspace);
    EXPECT_TRUE(snapshot.retainsMedia(*snapshot.withMonitorSelection(asset, selection)));
    std::vector<engine::MonitorSelectionSnapshot> alternatives(7, selection);
    alternatives[0].media_id.bytes[0] = 4;
    alternatives[1].tracks[0].track_id.bytes[0] = 5;
    alternatives[2].tracks[0].gain_linear = 0.5f;
    alternatives[3].tracks[0].muted = true;
    alternatives[4].overlap_confirmed = true;
    auto other_binding = std::make_shared<engine::UnverifiedAudioAssetBinding>(*binding);
    other_binding->sha256[0] = 6;
    alternatives[5].unverified_asset_binding = std::move(other_binding);
    alternatives[6].tracks.clear();
    for (size_t index = 0; index < alternatives.size(); ++index) {
        SCOPED_TRACE(index);
        EXPECT_FALSE(snapshot.retainsMedia(*snapshot.withMonitorSelection(asset, alternatives[index])));
    }
}
TEST(EditSourceReplacementAdapter, RetiredRedoBindingReleasesAfterItsIndependentLeaseOwner) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto replacement = directory.filePath(QStringLiteral("retired-blue.mkv"));
    ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("blue.mkv")), replacement));
    EditSessionAdapter session;
    session.importMedia(QUrl::fromLocalFile(QDir(fixtures).filePath(QStringLiteral("red.mkv"))), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
    session.beginSourceReplacement(asset, QUrl::fromLocalFile(replacement), true);
    ASSERT_TRUE(
        WaitForMediaCondition([&] { return session.sourceReplacement().value(QStringLiteral("canApply")).toBool(); }));
    ASSERT_TRUE(session.applySourceReplacement());
    ASSERT_TRUE(
        WaitForMediaCondition([&] { return !session.sourceReplacement().value(QStringLiteral("active")).toBool(); }));
    const auto adopted_version = session.workspace().asset(asset)->source_version;
    auto external_lease = session.sourceAccess(asset);
    std::weak_ptr<const engine::MediaReadCapability> retired = external_lease;
    ASSERT_NE(external_lease, nullptr);
    session.undo();
    ASSERT_EQ(session.workspace().asset(asset)->source_version, 0U);
    EXPECT_TRUE(session.workspace().retainedSourceVersions().contains({asset, adopted_version}));
    session.appendAsset(asset);
    EXPECT_FALSE(session.canRedo());
    EXPECT_FALSE(session.workspace().retainedSourceVersions().contains({asset, adopted_version}));
    EXPECT_FALSE(retired.expired());
    EXPECT_EQ(external_lease->Path(), std::filesystem::path(replacement.toStdWString()));
    external_lease.reset();
    EXPECT_TRUE(WaitForMediaCondition([&] { return retired.expired(); }));
}
TEST(EditPlaybackSourceIntegrity, FirstProofOnTheSameHeldBytesKeepsMediaReusableWithoutAnAuthoredRevision) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("first-proof.mkv"));
    ASSERT_TRUE(QFile::copy(QDir(fixtures).filePath(QStringLiteral("red.mkv")), path));
    EditSessionAdapter session;
    session.setBackgroundAnalysisSuspended(true);
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    const auto access = session.sourceAccess(asset);
    ASSERT_NE(access, nullptr);
    ASSERT_NE(session.sourceMetadata(asset), nullptr);
    EXPECT_EQ(session.sourceProof(asset), nullptr);
    const auto revision = session.workspace().revision();
    const EditPlaybackSnapshot provisional(
        session.workspace(),
        {{asset, std::filesystem::path(path.toStdWString()), *session.sourceMetadata(asset), {}, access, {}}});
    session.setBackgroundAnalysisSuspended(false);
    ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
    const EditPlaybackSnapshot verified(session.workspace(), {{asset,
                                                               std::filesystem::path(path.toStdWString()),
                                                               *session.sourceMetadata(asset),
                                                               {},
                                                               session.sourceAccess(asset),
                                                               session.sourceProof(asset)}});
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_EQ(session.sourceAccess(asset), access);
    EXPECT_TRUE(provisional.retainsMedia(verified));
    EXPECT_EQ(access->Measurements().hash_starts, 1U);
}
std::optional<engine::AudioManifest1> WriteRecordedDefaultFixture(const QString& path) {
    const auto metadata = engine::ProbeEditMedia(std::filesystem::path(path.toStdWString()));
    if (!metadata.error.empty() || metadata.audio_metadata.actual_tracks.size() != 2)
        return std::nullopt;
    engine::AudioManifest1 manifest;
    manifest.media_id = engine::NewId<engine::MediaId>();
    manifest.recording_id = engine::NewId<engine::RecordingId>();
    manifest.segment_id = engine::NewId<engine::SegmentId>();
    engine::AudioManifestSource source;
    source.id = engine::NewId<engine::AudioSourceId>();
    manifest.sources.push_back(source);
    for (size_t index = 0; index < metadata.audio_metadata.actual_tracks.size(); ++index) {
        engine::AudioManifestTrack track;
        track.container = metadata.audio_metadata.actual_tracks[index];
        track.container.role = index == 0 ? "mix" : "stem";
        track.container.source_ids = {source.id};
        track.routes.push_back({engine::NewId<engine::AudioRouteId>(), source.id, 1.0f});
        manifest.tracks.push_back(std::move(track));
    }
    manifest.default_monitor_track_ids = {manifest.tracks.front().container.track_id};
    if (!engine::WriteMediaIdentitySidecar(std::filesystem::path(path.toStdWString()), manifest).succeeded())
        return std::nullopt;
    return manifest;
}

TEST(EditRecordedDefault, VerifiedStartingDefaultFinalizesWithoutAnAuthoredStepAndSurvivesMoveUndoRedo) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("recorded-default.mkv"));
    ASSERT_TRUE(QFile::copy(test::TransportAmbiguousFixture(), path));
    const auto manifest = WriteRecordedDefaultFixture(path);
    ASSERT_TRUE(manifest);
    EditSessionAdapter session;
    session.setBackgroundAnalysisSuspended(true);
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    ASSERT_NE(session.sourceMetadata(asset), nullptr);
    ASSERT_EQ(session.sourceMetadata(asset)->audio_metadata.status, engine::AudioMetadataStatus::Pending);
    ASSERT_TRUE(session.workspace().asset(asset)->mix_decision_required);
    EXPECT_EQ(session.sourceProof(asset), nullptr);
    for (const auto& clip : session.workspace().clips())
        if (clip.stream && clip.stream->kind == engine::TimelineStreamKind::Audio) {
            EXPECT_FALSE(clip.enabled);
            EXPECT_FALSE(clip.original_enabled);
        }
    const auto video =
        std::find_if(session.workspace().clips().begin(), session.workspace().clips().end(), [](const auto& clip) {
            return clip.stream && clip.stream->kind == engine::TimelineStreamKind::Video;
        });
    ASSERT_NE(video, session.workspace().clips().end());
    const bool initially_can_undo = session.canUndo();
    session.moveClipUs(video->id, 1'000'000, false);
    ASSERT_TRUE(session.canUndo());
    const auto moved = session.workspace().clips();
    const auto authored_revision = session.workspace().revision();
    session.setBackgroundAnalysisSuspended(false);
    ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
    ASSERT_EQ(session.sourceMetadata(asset)->audio_metadata.status, engine::AudioMetadataStatus::Verified);
    ASSERT_TRUE(session.sourceMetadata(asset)->audio_metadata.manifest);
    EXPECT_EQ(session.workspace().revision(), authored_revision);
    ASSERT_FALSE(session.workspace().asset(asset)->mix_decision_required);
    ASSERT_TRUE(session.workspace().asset(asset)->input_mix_selection);
    EXPECT_EQ(session.workspace().asset(asset)->input_mix_selection->authority,
              edit::InputMixAuthority::VerifiedRecordedDefault);
    EXPECT_EQ(session.workspace().asset(asset)->input_mix_selection->selected_stream_identities,
              manifest->default_monitor_track_ids);
    const auto assert_defaults = [&] {
        for (const auto& clip : session.workspace().clips())
            if (clip.stream && clip.stream->kind == engine::TimelineStreamKind::Audio) {
                const bool expected = clip.stream->stream_identity == manifest->default_monitor_track_ids.front();
                EXPECT_EQ(clip.enabled, expected);
                EXPECT_EQ(clip.original_enabled, expected);
            }
        EXPECT_FALSE(session.workspace().asset(asset)->mix_decision_required);
        EXPECT_EQ(session.workspace().asset(asset)->input_mix_selection->authority,
                  edit::InputMixAuthority::VerifiedRecordedDefault);
    };
    assert_defaults();
    session.undo();
    assert_defaults();
    EXPECT_EQ(session.canUndo(), initially_can_undo);
    session.redo();
    assert_defaults();
    for (const auto& clip : moved)
        EXPECT_EQ(session.workspace().clip(clip.id)->start, clip.start);
    EXPECT_EQ(session.sourceAccess(asset)->Measurements().hash_starts, 1U);
}

TEST(EditRecordedDefault, ExplicitChoiceBeforeProofStaysCurrentAndUndoReturnsTheVerifiedRecordedDefault) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("recorded-explicit-choice.mkv"));
    ASSERT_TRUE(QFile::copy(test::TransportAmbiguousFixture(), path));
    const auto manifest = WriteRecordedDefaultFixture(path);
    ASSERT_TRUE(manifest);
    EditSessionAdapter session;
    session.setBackgroundAnalysisSuspended(true);
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending(); }));
    ASSERT_EQ(session.media().size(), 1);
    const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
    ASSERT_EQ(session.sourceMetadata(asset)->audio_metadata.status, engine::AudioMetadataStatus::Pending);
    ASSERT_TRUE(session.beginSourceAudioChoice(asset));
    const auto alternative = session.sourceAudioChoice().value(QStringLiteral("tracks")).toList().back().toMap();
    session.setSourceAudioTrack(alternative.value(QStringLiteral("id")).toString(), true);
    ASSERT_TRUE(session.applySourceAudioChoice());
    ASSERT_TRUE(
        WaitForMediaCondition([&] { return !session.sourceAudioChoice().value(QStringLiteral("active")).toBool(); }));
    EXPECT_EQ(session.sourceProof(asset), nullptr);
    const auto explicit_choice = session.workspace().asset(asset)->input_mix_selection;
    ASSERT_TRUE(explicit_choice);
    EXPECT_EQ(explicit_choice->authority, edit::InputMixAuthority::UserConfirmed);
    EXPECT_NE(explicit_choice->selected_stream_identities, manifest->default_monitor_track_ids);
    const auto authored_revision = session.workspace().revision();
    session.setBackgroundAnalysisSuspended(false);
    ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
    ASSERT_EQ(session.sourceMetadata(asset)->audio_metadata.status, engine::AudioMetadataStatus::Verified);
    EXPECT_EQ(session.workspace().revision(), authored_revision);
    EXPECT_EQ(session.workspace().asset(asset)->input_mix_selection, explicit_choice);
    session.undo();
    ASSERT_FALSE(session.workspace().asset(asset)->mix_decision_required);
    ASSERT_TRUE(session.workspace().asset(asset)->input_mix_selection);
    EXPECT_EQ(session.workspace().asset(asset)->input_mix_selection->authority,
              edit::InputMixAuthority::VerifiedRecordedDefault);
    EXPECT_EQ(session.workspace().asset(asset)->input_mix_selection->selected_stream_identities,
              manifest->default_monitor_track_ids);
    session.redo();
    EXPECT_EQ(session.workspace().asset(asset)->input_mix_selection, explicit_choice);
    EXPECT_EQ(session.sourceAccess(asset)->Measurements().hash_starts, 1U);
}
TEST(EditRecordedDefault, ActiveMoveRangeAndCapturedPasteKeepTheirExactCandidateThroughProofFinalization) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        const auto path = directory.filePath(QStringLiteral("captured-default-%1.mkv").arg(mode));
        ASSERT_TRUE(QFile::copy(test::TransportAmbiguousFixture(), path));
        const auto manifest = WriteRecordedDefaultFixture(path);
        ASSERT_TRUE(manifest);
        EditSessionAdapter session;
        session.setBackgroundAnalysisSuspended(true);
        session.importMedia(QUrl::fromLocalFile(path), true);
        ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending(); }));
        const auto asset = session.media().front().toMap().value(QStringLiteral("id")).toULongLong();
        ASSERT_EQ(session.sourceMetadata(asset)->audio_metadata.status, engine::AudioMetadataStatus::Pending);
        const auto revision = session.workspace().revision();
        QVariantList tracks;
        for (const auto& track : session.workspace().tracks())
            tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
        if (mode == 0) {
            session.beginClipDrag(session.workspace().clips().front().id);
            session.updateClipDragUs(1'000'000, 0, -1, false, 0);
            ASSERT_TRUE(session.dragValid());
        } else if (mode == 1) {
            ASSERT_TRUE(session.beginRangeGestureUs(0, tracks));
            session.updateRangeGestureUs(500'000);
            ASSERT_TRUE(session.rangeGestureValid());
        } else {
            session.selectAllClips();
            ASSERT_TRUE(session.copyClips());
            ASSERT_TRUE(session.previewPasteClipsUs(session.durationUs() + 1'000'000));
        }
        const auto clip_candidate = session.dragClips();
        const auto range_candidate = session.rangeGestureCandidate();
        session.setBackgroundAnalysisSuspended(false);
        ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
        EXPECT_EQ(session.workspace().revision(), revision);
        ASSERT_FALSE(session.workspace().asset(asset)->mix_decision_required);
        if (mode == 1) {
            EXPECT_TRUE(session.rangeGestureActive());
            EXPECT_EQ(session.rangeGestureCandidate(), range_candidate);
            ASSERT_TRUE(session.commitRangeGesture());
        } else {
            EXPECT_TRUE(session.dragActive());
            EXPECT_TRUE(session.dragValid());
            EXPECT_EQ(session.dragClips(), clip_candidate);
            ASSERT_TRUE(session.commitClipDrag());
        }
        for (const auto& clip : session.workspace().clips())
            if (clip.stream && clip.stream->kind == engine::TimelineStreamKind::Audio) {
                const bool selected = clip.stream->stream_identity == manifest->default_monitor_track_ids.front();
                EXPECT_EQ(clip.enabled, selected);
                EXPECT_EQ(clip.original_enabled, selected);
            }
        const auto controls = session.workspace().monitoringRevision();
        session.retrySourceVerification(asset);
        QCoreApplication::processEvents();
        EXPECT_EQ(session.workspace().monitoringRevision(), controls);
        EXPECT_EQ(session.sourceAccess(asset)->Measurements().hash_starts, 1U);
    }
}
TEST(EditRemainingContracts, FadeDurationAndDragPreserveTheStoredCurveAndOppositeEnd) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    const auto id = session.workspace().clips().front().id;
    ASSERT_TRUE(EditSessionAdapterTestPeer::setFade(session, id, true, {1'000'000, {engine::CurveKind::Bias, .5}}));
    ASSERT_TRUE(EditSessionAdapterTestPeer::setFade(session, id, false, {2'000'000, {engine::CurveKind::Smooth, -.2}}));
    const auto opposite = session.workspace().clip(id)->fade_out;
    ASSERT_TRUE(session.applyTimeValue(id, QStringLiteral("fadeIn"), 1'500'000));
    EXPECT_EQ(session.workspace().clip(id)->fade_in.curve, (edit::CurveSpec{engine::CurveKind::Bias, .5}));
    EXPECT_EQ(session.workspace().clip(id)->fade_out, opposite);
    session.setClipFade(id, true, 2000);
    EXPECT_EQ(session.workspace().clip(id)->fade_in.curve, (edit::CurveSpec{engine::CurveKind::Bias, .5}));
    session.beginClipDrag(id, 2);
    session.updateClipDragUs(500'000);
    ASSERT_TRUE(session.dragValid());
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.workspace().clip(id)->fade_in.curve, (edit::CurveSpec{engine::CurveKind::Bias, .5}));
    EXPECT_EQ(session.workspace().clip(id)->fade_out, opposite);
}

TEST(EditRemainingContracts, FadeCurveControlsAreAtomicValidatedAndUndoable) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    const auto id = session.workspace().clips().front().id;
    session.setClipFade(id, true, 1000);
    ASSERT_TRUE(session.setClipFadeCurve(id, true, static_cast<int>(engine::CurveKind::Bias), -.5));
    EXPECT_EQ(session.workspace().clip(id)->fade_in.duration_us, 1'000'000);
    EXPECT_EQ(session.workspace().clip(id)->fade_in.curve, (edit::CurveSpec{engine::CurveKind::Bias, -.5}));
    const auto revision = session.workspace().revision();
    EXPECT_FALSE(session.setClipFadeCurve(id, true, 99, 0));
    EXPECT_FALSE(session.setClipFadeCurve(id, true, 2, 1.1));
    EXPECT_EQ(session.workspace().revision(), revision);
    session.undo();
    EXPECT_EQ(session.workspace().clip(id)->fade_in.curve, edit::CurveSpec{});
    session.setTrackLocked(session.workspace().clip(id)->track, true);
    EXPECT_FALSE(session.setClipFadeCurve(id, true, 1, 0));
}

TEST(EditRemainingContracts, InspectionChoosesExactlyOneClipWithoutChangingTransportOrAuthoredState) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    const auto id = session.workspace().clips().front().id;
    session.selectAllClips();
    session.requestSeekUs(900'000);
    const auto revision = session.workspace().revision();
    ASSERT_TRUE(session.inspectClip(id));
    EXPECT_EQ(session.selectedClipIds(), (QVariantList{QVariant::fromValue<qulonglong>(id)}));
    EXPECT_EQ(session.positionUs(), 900'000);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_FALSE(session.inspectClip(9999999));
    EXPECT_EQ(session.selectedClipIds(), (QVariantList{QVariant::fromValue<qulonglong>(id)}));
}

TEST(EditRemainingContracts, AllTracksRemovalIsAnExplicitCompleteCandidateWithCancelAndCommit) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    const auto original = session.workspace().clips();
    const auto scope = QVariantList{QVariant::fromValue<qulonglong>(original.front().track)};
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, scope));
    const auto range = session.editRange();
    session.setRipple(true);
    session.deleteSelected(true);
    EXPECT_EQ(session.workspace().clips(), original);
    ASSERT_TRUE(session.previewRemoveTimeOnAllTracks());
    EXPECT_TRUE(session.dragValid());
    EXPECT_EQ(session.workspace().clips(), original);
    EXPECT_EQ(session.editRange(), range);
    session.cancelClipDrag();
    EXPECT_EQ(session.editRange(), range);
    ASSERT_TRUE(session.previewRemoveTimeOnAllTracks());
    ASSERT_TRUE(session.commitClipDrag());
    EXPECT_EQ(session.durationUs(), 99'000'000);
    EXPECT_EQ(session.selectionMode(), EditSessionAdapter::NoSelection);
    EXPECT_EQ(session.positionUs(), 1'000'000);
    session.undo();
    EXPECT_EQ(session.workspace().clips(), original);
    ASSERT_TRUE(session.setEditRangeUs(1'000'000, 2'000'000, scope));
    ASSERT_TRUE(session.setTrackLocked(original.back().track, true));
    EXPECT_FALSE(session.previewRemoveTimeOnAllTracks());
    EXPECT_EQ(session.workspace().clips(), original);
}

TEST(EditRemainingContracts, OccupiedFitBoundsExcludeEmptyTailAndDoNotChangeTheWorkspace) {
    EnsureApplication();
    EditSessionAdapter session;
    EXPECT_FALSE(session.occupiedBounds().value("valid").toBool());
    session.setEditContext(MakeContext());
    const auto id = session.workspace().clips().front().id;
    ASSERT_TRUE(session.applyTimeValue(id, QStringLiteral("clipStart"), 2'000'000));
    session.setFixedTimelineEnd(true, 120000);
    const auto revision = session.workspace().revision();
    const auto bounds = session.occupiedBounds();
    ASSERT_TRUE(bounds.value("valid").toBool());
    EXPECT_EQ(bounds.value("startUs").toLongLong(), 2'000'000);
    EXPECT_EQ(bounds.value("endUs").toLongLong(), 102'000'000);
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_FALSE(session.applyRangeGain(1));
}
TEST(EditThumbnailScheduling, PixelResizingReusesFivePosterRasterVariants) {
    EnsureApplication();
    EditTimelineAdapter timeline;
    ThumbnailRequestBroker::Config config;
    config.disk_enabled = false;
    timeline.configureThumbnailCache(config);
    std::array<uint8_t, 32> digest{};
    digest.fill(90);
    const auto identity = QString::fromStdString(engine::ToHex(digest));
    timeline.thumbnailBroker()->RegisterSource(identity.toStdString(),
                                               ThumbnailSourceInfo{"raster-only.mkv", digest, 1920, 1080, 30, 1});
    QStringList variants;
    for (int height = 1; height <= 256; ++height) {
        const auto url = timeline.posterForIdentity(identity, height);
        ASSERT_FALSE(url.isEmpty());
        if (!variants.contains(url))
            variants.push_back(url);
        EXPECT_EQ(timeline.posterForIdentity(identity, height), url);
    }
    EXPECT_EQ(variants.size(), 5);
}

TEST(EditThumbnailScheduling, SparseCoverageStartsAtTheVisibleCenterAfterBoundaries) {
    EnsureApplication();
    EditTimelineAdapter timeline;
    ThumbnailRequestBroker::Config config;
    config.disk_enabled = false;
    timeline.configureThumbnailCache(config);
    const auto broker = timeline.thumbnailBroker();
    broker->AttachWorker([] {});
    std::array<uint8_t, 32> digest{};
    digest.fill(91);
    const auto identity = engine::ToHex(digest);
    broker->RegisterSource(identity, ThumbnailSourceInfo{"queue-only.mkv", digest, 1920, 1080, 30, 1});
    const auto tiles = timeline.thumbnailTilesForClip(1, QString::fromStdString(identity), 0, 100000000, 6000, 48, 1);
    ASSERT_GT(tiles.size(), 16);
    qint64 expected = -1;
    qint64 distance = (std::numeric_limits<qint64>::max)();
    for (qsizetype index = 4; index + 1 < tiles.size(); index += 4) {
        const auto time = tiles[index].toMap().value("timeUs").toLongLong();
        if (std::abs(time - 50000000) < distance) {
            expected = time;
            distance = std::abs(time - 50000000);
        }
    }
    ASSERT_GE(expected, 0);
    for (int count = 0; count < 4; ++count) {
        const auto job = broker->TakeJob();
        ASSERT_TRUE(job);
        if (job->priority == 2) {
            EXPECT_EQ(job->time_us, expected);
            return;
        }
        ASSERT_EQ(job->priority, 1);
        broker->Complete(*job, {});
    }
    FAIL() << "No sparse visible job after both boundary jobs";
}

TEST(EditThumbnailProduction, ContextAndKeyframePublicationDoNotStartAnIndependentStripDecoder) {
    EnsureApplication();
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to real transport media.";
    const auto path = QDir(fixtures).filePath(QStringLiteral("red.mkv"));
    const auto metadata = engine::ProbeEditMedia(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    EditSessionAdapter session;
    EditTimelineAdapter timeline;
    timeline.setSession(&session);
    const auto identity = QString::fromStdString(engine::ToHex(metadata.content_sha256));
    ASSERT_TRUE(timeline.registerThumbnailSource(identity, path, metadata.width, metadata.height, metadata.fps_num,
                                                 metadata.fps_den));
    auto* source = timeline.thumbnailSourceForTest();
    ASSERT_NE(source, nullptr);
    int legacy_opens = 0;
    int legacy_tiles = 0;
    QObject::connect(source, &TimelineThumbnailSource::clipOpened, &session,
                     [&](int, int, const QStringList&) { ++legacy_opens; });
    QObject::connect(source, &TimelineThumbnailSource::tileReady, &session,
                     [&](qint64, const QImage&, quint64) { ++legacy_tiles; });
    auto context = MakeContext(metadata.duration_us / 1000000.0);
    context.mkv_master_path = path;
    session.setEditContext(context);
    QElapsedTimer context_timeout;
    context_timeout.start();
    while (timeline.sourcePath() != path && context_timeout.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    ASSERT_EQ(timeline.sourcePath(), path);
    session.setKeyframeTimestampsForTest({0, 1000000});
    timeline.setTrackWidth(900);
    ASSERT_FALSE(session.workspace().assets().empty());
    const auto asset_id = session.workspace().assets().front().id;
    context_timeout.restart();
    while (!session.sourceProof(asset_id) && context_timeout.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    ASSERT_TRUE(session.sourceProof(asset_id));
    const auto current_identity = session.sourcePreviewIdentity(asset_id);
    std::atomic<bool> received{false};
    timeline.thumbnailBroker()->Request(current_identity.toStdString(), 0, 48,
                                        [&](const QImage& image) { received.store(!image.isNull()); });
    QElapsedTimer timeout;
    timeout.start();
    while (!received.load() && timeout.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    ASSERT_TRUE(received.load());
    QCoreApplication::processEvents();
    EXPECT_EQ(legacy_opens, 0);
    EXPECT_EQ(legacy_tiles, 0);
}
TEST(EditRemainingContracts, CurveBiasPercentParserIsLocaleAwareStrictAndBounded) {
    EnsureApplication();
    EditSessionAdapter session;
    for (const auto& input : {QStringLiteral("-12.5%"), QStringLiteral("-12,5 %")}) {
        const auto result = session.parseCurveBiasValue(input);
        ASSERT_TRUE(result.value("valid").toBool());
        EXPECT_DOUBLE_EQ(result.value("value").toDouble(), -.125);
    }
    EXPECT_DOUBLE_EQ(session.parseCurveBiasValue(QStringLiteral("100")).value("value").toDouble(), 1);
    for (const auto& input : {QStringLiteral("100.1"), QStringLiteral("-101%"), QStringLiteral("NaN"),
                              QStringLiteral("1,2.3"), QStringLiteral("1e2"), QStringLiteral("1% junk")})
        EXPECT_FALSE(session.parseCurveBiasValue(input).value("valid").toBool()) << input.toStdString();
}

TEST(EditRemainingContracts, JunctionCurveTargetsAnExplicitIdAndPreservesItsWindow) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext());
    const auto id = session.workspace().clips().front().id;
    session.requestSeekUs(50'000'000);
    session.splitSelected();
    ASSERT_TRUE(session.previewCrossfadeAtCut(id));
    ASSERT_TRUE(session.commitClipDrag());
    const auto junction = session.workspace().transitions().front();
    ASSERT_TRUE(session.setJunctionCurveFor(junction.id, static_cast<int>(engine::CurveKind::Bias), .5));
    const auto& changed = session.workspace().transitions().front();
    EXPECT_EQ(changed.in_us, junction.in_us);
    EXPECT_EQ(changed.out_us, junction.out_us);
    EXPECT_EQ(changed.curve, (edit::CurveSpec{engine::CurveKind::Bias, .5}));
    const auto revision = session.workspace().revision();
    EXPECT_FALSE(session.setJunctionCurveFor(9999999, 1, 0));
    EXPECT_EQ(session.workspace().revision(), revision);
    session.undo();
    EXPECT_EQ(session.workspace().transitions().front().curve, junction.curve);
}
TEST(EditThumbnailProduction, VisibleCoverageCountsBrokerKeysAndTerminalResults) {
    EnsureApplication();
    EditTimelineAdapter timeline;
    ThumbnailRequestBroker::Config config;
    config.disk_enabled = false;
    timeline.configureThumbnailCache(config);
    const auto broker = timeline.thumbnailBroker();
    broker->AttachWorker([] {});
    std::array<uint8_t, 32> digest{};
    digest.fill(92);
    const auto identity = engine::ToHex(digest);
    broker->RegisterSource(identity, ThumbnailSourceInfo{"coverage-only.mkv", digest, 1920, 1080, 30, 1});
    const auto rows =
        timeline.thumbnailTilesForClip(31, QString::fromStdString(identity), 10000000, 20000000, 900, 48, 1);
    ASSERT_GT(rows.size(), 2);
    EXPECT_EQ(timeline.tilesExpected(), rows.size());
    EXPECT_EQ(timeline.tilesReady(), 0);
    EXPECT_EQ(timeline.previewState(), QStringLiteral("generating"));
    QImage image(8, 8, QImage::Format_ARGB32);
    image.fill(Qt::red);
    while (const auto job = broker->TakeJob())
        broker->Complete(*job, job->priority <= 3 ? image : QImage());
    EXPECT_EQ(timeline.tilesReady(), rows.size());
    EXPECT_EQ(timeline.previewState(), QStringLiteral("ready"));
    timeline.updateThumbnailRelevance(31, 15000000, -1);
    EXPECT_EQ(timeline.tilesExpected(), rows.size());
    EXPECT_EQ(timeline.tilesReady(), rows.size());
    timeline.retireThumbnailInterest(31);
    EXPECT_EQ(timeline.tilesExpected(), 0);
    EXPECT_EQ(timeline.previewState(), QStringLiteral("idle"));
    timeline.thumbnailTilesForClip(32, QString::fromStdString(identity), 30000000, 40000000, 900, 48, 1);
    while (const auto job = broker->TakeJob())
        broker->Complete(*job, {});
    EXPECT_TRUE(timeline.previewsUnavailable());
    EXPECT_EQ(timeline.previewState(), QStringLiteral("unavailable"));
    timeline.thumbnailTilesForClip(33, QString::fromStdString(identity), 30000000, 40000000, 900, 48, 1);
    EXPECT_EQ(timeline.previewState(), QStringLiteral("generating"));
    EXPECT_FALSE(timeline.previewsUnavailable());
    while (const auto job = broker->TakeJob())
        broker->Complete(*job, image);
    EXPECT_EQ(timeline.previewState(), QStringLiteral("ready"));
    timeline.clearThumbnailCache();
    EXPECT_EQ(timeline.tilesExpected(), 0);
}
TEST(EditRemovalPreview, CompleteDeletionRetainsOriginalDonorsAndPublishesActualCandidateDuration) {
    EnsureApplication();
    EditSessionAdapter session;
    session.setEditContext(MakeContext(10));
    QVariantList tracks, originals;
    for (const auto& track : session.workspace().tracks())
        tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    for (const auto& clip : session.workspace().clips())
        originals.push_back(QVariant::fromValue<qulonglong>(clip.id));
    ASSERT_TRUE(session.setEditRangeUs(0, 10'000'000, tracks));
    ASSERT_TRUE(session.previewRemoveTimeOnAllTracks());
    ASSERT_TRUE(session.dragValid());
    EXPECT_TRUE(session.dragClips().empty());
    EXPECT_EQ(session.dragDonorIds().value("clips").toList(), originals);
    EXPECT_EQ(session.dragDurationUs(), 0);
    EXPECT_EQ(session.durationUs(), 10'000'000);
    const auto bounds = session.dragBounds();
    EXPECT_TRUE(bounds.value("valid").toBool());
    EXPECT_EQ(bounds.value("startUs").toLongLong(), 0);
    EXPECT_EQ(bounds.value("endUs").toLongLong(), 10'000'000);
    session.cancelClipDrag();
    EXPECT_TRUE(session.dragDonorIds().value("clips").toList().empty());
    EXPECT_EQ(session.dragDurationUs(), session.durationUs());
}
TEST(EditThumbnailScheduling, ExtendingTrimPromotesTheExistingLookaheadTileToUrgent) {
    EnsureApplication();
    EditTimelineAdapter timeline;
    ThumbnailRequestBroker::Config config;
    config.disk_enabled = false;
    timeline.configureThumbnailCache(config);
    const auto broker = timeline.thumbnailBroker();
    broker->AttachWorker([] {});
    std::array<uint8_t, 32> digest{};
    digest.fill(93);
    const auto identity = engine::ToHex(digest);
    broker->RegisterSource(identity, ThumbnailSourceInfo{"trim-only.mkv", digest, 1920, 1080, 30, 1});
    ASSERT_FALSE(
        timeline.thumbnailTilesForClip(44, QString::fromStdString(identity), 10000000, 20000000, 900, 48, 1).isEmpty());
    timeline.updateThumbnailRelevance(44, -1, 5000000);
    std::optional<ThumbnailRequestBroker::Job> nearest;
    while (const auto job = broker->TakeJob()) {
        if (!nearest || std::abs(job->time_us - 5000000) < std::abs(nearest->time_us - 5000000))
            nearest = job;
        broker->Complete(*job, {});
    }
    ASSERT_TRUE(nearest);
    ASSERT_LT(nearest->time_us, 10000000);
    EXPECT_EQ(nearest->priority, 1);
}
TEST(EditAudioOnlyTimeFormat, DefaultUsesExactSecondsWithoutInventingVideoRate) {
    EnsureApplication();
    EditSessionAdapter session;
    engine::EditMediaMetadata metadata;
    metadata.duration_us = 2'000'000;
    metadata.has_audio = true;
    metadata.content_identity_valid = true;
    metadata.content_sha256.fill(51);
    for (const auto channels : {1u, 2u}) {
        engine::EditSourceStream stream;
        stream.kind = engine::TimelineStreamKind::Audio;
        stream.container_index = channels;
        stream.codec.channels = channels;
        metadata.streams.push_back(stream);
    }
    ASSERT_TRUE(session.addProbedMediaForHarness(QStringLiteral("audio-only.mka"), metadata, true));
    ASSERT_FALSE(session.workspace().snapshot()->output_fps.valid());
    EXPECT_EQ(session.formatTimeValue(1'234'567), QStringLiteral("1.234567"));
    EXPECT_EQ(session.property("defaultTimeFormat").toInt(), 1);
    EXPECT_EQ(session.formatTimeValue(1'000'000, 2), QStringLiteral("48000"));
    EXPECT_EQ(session.formatTimeValue(1'234'567, 1), QStringLiteral("1.234567"));
    EXPECT_TRUE(session.formatTimeValue(1'000'000, -1).isEmpty());
    EXPECT_FALSE(session.parseTimeValue(QStringLiteral("1f"), 0, false, 0).value("valid").toBool());
    EXPECT_FALSE(session.workspace().snapshot()->output_fps.valid());
}

TEST(EditAudioOnlyTimeFormat, ValidNtscRetainsNondropTimecodeAndStrictFormatValidation) {
    EnsureApplication();
    EditSessionAdapter session;
    engine::EditMediaMetadata metadata;
    metadata.duration_us = 2'002'000;
    metadata.fps_num = 30000;
    metadata.fps_den = 1001;
    metadata.content_identity_valid = true;
    metadata.content_sha256.fill(52);
    engine::EditSourceStream video;
    video.kind = engine::TimelineStreamKind::Video;
    video.container_index = 0;
    video.fps_num = 30000;
    video.fps_den = 1001;
    metadata.streams.push_back(video);
    ASSERT_TRUE(session.addProbedMediaForHarness(QStringLiteral("ntsc-video.mkv"), metadata, true));
    EXPECT_EQ(session.property("defaultTimeFormat").toInt(), 0);
    EXPECT_EQ(session.formatTimeValue(1'001'000), QStringLiteral("00:00:01:00"));
    EXPECT_TRUE(session.formatTimeValue(1'000'000, 3).isEmpty());
    EXPECT_FALSE(session.parseTimeValue(QStringLiteral("1s"), 0, false, 3).value("valid").toBool());
}
TEST(EditVerificationPriority, ExportTakesOverPausedOptionalLeaderWithoutResumingAnalysis) {
    EnsureApplication();
    QTemporaryDir directory;
    QTemporaryDir output;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(output.isValid());
    const QString fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString path = directory.filePath(QStringLiteral("source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, path));
    const auto acquired = engine::AcquireMediaReadCapability(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(acquired.succeeded());
    const auto access = *acquired.value();
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    EditSessionAdapter session;
    EditExportAdapter exporter;
    const auto cleanup = qScopeGuard([&] {
        release.store(true);
        exporter.cancelAndWait();
        session.close();
    });
    session.setBackgroundIoAdmission([&](uint64_t, const std::atomic<bool>* cancelled) {
        const auto initial = access->Measurements();
        if (initial.hash_starts == 0 || initial.hash_completions != 0)
            return true;
        held.store(true);
        while (!release.load() && (!cancelled || !cancelled->load()) && access->Measurements().hash_completions == 0)
            QThread::msleep(1);
        return !cancelled || !cancelled->load();
    });
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending() && held.load(); }));
    ASSERT_EQ(session.workspace().assets().size(), 1u);
    const auto asset = session.workspace().assets().front().id;
    ASSERT_EQ(session.sourceAccess(asset), access);
    ASSERT_FALSE(session.sourceProof(asset));
    ASSERT_EQ(access->Measurements().hash_completions, 0u);
    exporter.setSession(&session);
    exporter.setOutputDirectoryProvider([&] { return output.path(); });
    QObject::connect(&exporter, &EditExportAdapter::stateChanged, &session,
                     [&] { session.setBackgroundAnalysisSuspended(exporter.running()); });
    exporter.startExport();
    EXPECT_TRUE(WaitForMediaCondition([&] { return exporter.state() != EditExportAdapter::Preparing; }));
    EXPECT_EQ(exporter.state(), EditExportAdapter::Review) << exporter.errorText().toStdString();
    EXPECT_FALSE(release.load());
    EXPECT_EQ(access->Measurements().hash_completions, 1u);
    EXPECT_EQ(access->Measurements().hash_bytes, std::filesystem::file_size(access->Path()));
    EXPECT_GE(access->Measurements().hash_cancellations, 1u);
    ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
    EXPECT_EQ(session.sourceProof(asset)->Source(), access);
    EXPECT_EQ(access->Measurements().hash_completions, 1u);
    EXPECT_EQ(exporter.state(), EditExportAdapter::Review);
    session.moveClipUs(session.workspace().clips().front().id, 1'000'000, false);
    EXPECT_EQ(exporter.state(), EditExportAdapter::Options);
}
TEST(EditVerificationPriority, ResumeRetriesOnlyCurrentUnverifiedSourcesAndPreservesCompletedProofs) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString path = directory.filePath(QStringLiteral("source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, path));
    const auto acquired = engine::AcquireMediaReadCapability(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(acquired.succeeded());
    const auto access = *acquired.value();
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    EditSessionAdapter session;
    const auto cleanup = qScopeGuard([&] {
        release.store(true);
        session.close();
    });
    session.setBackgroundIoAdmission([&](uint64_t, const std::atomic<bool>* cancelled) {
        const auto measurements = access->Measurements();
        if (measurements.hash_starts == 0 || measurements.hash_cancellations != 0)
            return true;
        held.store(true);
        while (!release.load() && (!cancelled || !cancelled->load()) && access->Measurements().hash_cancellations == 0)
            QThread::msleep(1);
        return !cancelled || !cancelled->load();
    });
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending() && held.load(); }));
    ASSERT_EQ(session.workspace().assets().size(), 1u);
    const auto asset = session.workspace().assets().front().id;
    session.setBackgroundAnalysisSuspended(true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return access->Measurements().hash_cancellations == 1; }));
    EXPECT_FALSE(session.sourceVerificationFailed(asset));
    session.undo();
    ASSERT_TRUE(session.workspace().clips().empty());
    ASSERT_EQ(session.workspace().assets().size(), 1u);
    session.setBackgroundAnalysisSuspended(false);
    ASSERT_TRUE(WaitForMediaCondition([&] { return bool(session.sourceProof(asset)); }));
    const auto proof = session.sourceProof(asset);
    EXPECT_TRUE(session.workspace().clips().empty());
    session.redo();
    EXPECT_EQ(session.workspace().clips().size(), 1u);
    EXPECT_EQ(proof->Source(), access);
    const auto completed = access->Measurements();
    EXPECT_EQ(completed.hash_completions, 1u);
    for (int iteration = 0; iteration < 3; ++iteration) {
        session.setBackgroundAnalysisSuspended(true);
        session.setBackgroundAnalysisSuspended(false);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    EXPECT_EQ(session.sourceProof(asset), proof);
    EXPECT_EQ(access->Measurements().hash_starts, completed.hash_starts);
}

TEST(EditVerificationPriority, CloseRevokesTheVerificationBatchBeforeSameSourceReentry) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString path = directory.filePath(QStringLiteral("source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, path));
    const auto acquired = engine::AcquireMediaReadCapability(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(acquired.succeeded());
    const auto access = *acquired.value();
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    EditSessionAdapter session;
    const auto cleanup = qScopeGuard([&] {
        release.store(true);
        session.close();
    });
    session.setBackgroundIoAdmission([&](uint64_t, const std::atomic<bool>* cancelled) {
        const auto measurements = access->Measurements();
        if (measurements.hash_starts == 0 || measurements.hash_cancellations != 0)
            return true;
        held.store(true);
        while (!release.load() && (!cancelled || !cancelled->load()) && access->Measurements().hash_cancellations == 0)
            QThread::msleep(1);
        return !cancelled || !cancelled->load();
    });
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending() && held.load(); }));
    session.close();
    ASSERT_TRUE(session.workspace().assets().empty());
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] {
        return !session.importPending() && session.workspace().assets().size() == 1 &&
               bool(session.sourceProof(session.workspace().assets().front().id));
    }));
    const auto asset = session.workspace().assets().front().id;
    EXPECT_EQ(session.sourceProof(asset)->Source(), access);
    EXPECT_EQ(access->Measurements().hash_cancellations, 1u);
    EXPECT_EQ(access->Measurements().hash_completions, 1u);
    EXPECT_FALSE(session.sourceVerificationFailed(asset));
}

TEST(EditVerificationPriority, DestructionCancelsTheOptionalBatchInsideAdmission) {
    EnsureApplication();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString path = directory.filePath(QStringLiteral("source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, path));
    const auto acquired = engine::AcquireMediaReadCapability(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(acquired.succeeded());
    const auto access = *acquired.value();
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    auto session = std::make_unique<EditSessionAdapter>();
    const auto cleanup = qScopeGuard([&] {
        release.store(true);
        session.reset();
    });
    session->setBackgroundIoAdmission([&](uint64_t, const std::atomic<bool>* cancelled) {
        if (access->Measurements().hash_starts == 0)
            return true;
        held.store(true);
        held.store(true);
        while (!release.load() && (!cancelled || !cancelled->load()) && access->Measurements().hash_cancellations == 0)
            QThread::msleep(1);
        return !cancelled || !cancelled->load();
    });
    session->importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session->importPending() && held.load(); }));
    QElapsedTimer elapsed;
    elapsed.start();
    session.reset();
    EXPECT_LT(elapsed.elapsed(), 500);
    EXPECT_FALSE(release.load());
    EXPECT_EQ(access->Measurements().hash_cancellations, 1u);
    std::array<std::uint8_t, 4> header{};
    const auto read = access->ReadAt(0, header);
    ASSERT_TRUE(read.succeeded());
    EXPECT_EQ(*read.value(), header.size());
}
TEST(EditVerificationPriority, ExplicitSameSourceAdoptionCannotBecomeAPausedLeaderForRequiredExport) {
    EnsureApplication();
    QTemporaryDir directory;
    QTemporaryDir output;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(output.isValid());
    const QString fixture =
        QFileInfo(QString::fromUtf8(__FILE__))
            .dir()
            .absoluteFilePath(QStringLiteral("../../../libs/engine/tests/fixtures/reordered_h264.mp4"));
    const QString path = directory.filePath(QStringLiteral("source.mp4"));
    ASSERT_TRUE(QFile::copy(fixture, path));
    const auto acquired = engine::AcquireMediaReadCapability(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(acquired.succeeded());
    const auto access = *acquired.value();
    EditSessionAdapter session;
    EditExportAdapter exporter;
    const auto cleanup = qScopeGuard([&] {
        session.cancelSourceReplacement();
        exporter.cancelAndWait();
        session.close();
    });
    session.setBackgroundAnalysisSuspended(true);
    session.importMedia(QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return !session.importPending(); }));
    ASSERT_EQ(session.workspace().assets().size(), 1u);
    const auto asset = session.workspace().assets().front().id;
    const auto source_version = session.workspace().asset(asset)->source_version;
    ASSERT_EQ(access->Measurements().hash_starts, 0u);
    session.beginSourceReplacement(asset, QUrl::fromLocalFile(path), true);
    ASSERT_TRUE(WaitForMediaCondition([&] { return access->Measurements().hash_starts == 1; }));
    exporter.setSession(&session);
    exporter.setOutputDirectoryProvider([&] { return output.path(); });
    QObject::connect(&exporter, &EditExportAdapter::stateChanged, &session,
                     [&] { session.setBackgroundAnalysisSuspended(exporter.running()); });
    exporter.startExport();
    EXPECT_TRUE(WaitForMediaCondition([&] { return exporter.state() != EditExportAdapter::Preparing; }));
    EXPECT_EQ(exporter.state(), EditExportAdapter::Review) << exporter.errorText().toStdString();
    EXPECT_EQ(access->Measurements().hash_completions, 1u);
    EXPECT_TRUE(WaitForMediaCondition([&] {
        return session.sourceReplacement().value(QStringLiteral("stage")).toString() == QStringLiteral("ready");
    }));
    EXPECT_FALSE(session.sourceReplacement().isEmpty());
    EXPECT_EQ(session.workspace().asset(asset)->source_version, source_version);
}

class EditExportReviewSelection : public ::testing::TestWithParam<const char*> {};

TEST_P(EditExportReviewSelection, ReviewTracksOnlyItsSelectedClipScope) {
    EnsureApplication();
    QTemporaryDir directory;
    QTemporaryDir output;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(output.isValid());
    const auto fixtures = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
    if (fixtures.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated transport media.";
    const auto fixture = QDir(fixtures).filePath(QStringLiteral("red.mkv"));
    const auto first = directory.filePath(QStringLiteral("first.mkv"));
    const auto second = directory.filePath(QStringLiteral("second.mkv"));
    ASSERT_TRUE(QFile::copy(fixture, first));
    ASSERT_TRUE(QFile::copy(fixture, second));
    EditSessionAdapter session;
    EditExportAdapter exporter;
    session.autoCrossfade = false;
    session.linkedSelection = false;
    session.importMediaBatch({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)}, true);
    ASSERT_TRUE(WaitForMediaCondition([&] {
        return !session.importPending() && session.workspace().assets().size() == 2 &&
               std::all_of(session.workspace().assets().begin(), session.workspace().assets().end(),
                           [&](const auto& asset) { return bool(session.sourceProof(asset.id)); });
    }));
    std::vector<edit::Id> video_ids;
    for (const auto& clip : session.workspace().clips())
        if (clip.stream && clip.stream->kind == engine::TimelineStreamKind::Video)
            video_ids.push_back(clip.id);
    ASSERT_EQ(video_ids.size(), 2u);
    const auto a = video_ids.front();
    const auto b = video_ids.back();
    const std::string_view scenario = GetParam();
    session.selectClip(a);
    if (scenario == "reordered")
        session.selectClipWithModifiers(b, true);
    exporter.setSession(&session);
    exporter.setSpanKey(scenario == "entire" ? QStringLiteral("entire") : QStringLiteral("selected"));
    exporter.setOutputDirectoryProvider([&] { return output.path(); });
    exporter.startExport();
    ASSERT_TRUE(WaitForMediaCondition([&] { return exporter.state() != EditExportAdapter::Preparing; }));
    ASSERT_EQ(exporter.state(), EditExportAdapter::Review) << exporter.errorText().toStdString();
    const auto revision = session.workspace().revision();
    const auto review = exporter.reviewOutcome();
    if (scenario == "confirmation") {
        {
            const QSignalBlocker deferred_notification(&session);
            session.selectClip(b);
        }
        ASSERT_EQ(exporter.state(), EditExportAdapter::Review);
        exporter.confirmPreparedExport();
        EXPECT_EQ(exporter.state(), EditExportAdapter::Preparing);
        exporter.cancelAndWait();
    } else if (scenario == "reordered") {
        {
            const QSignalBlocker coalesced_notification(&session);
            session.clearSelection();
            session.selectClip(b);
            session.selectClipWithModifiers(a, true);
        }
        ASSERT_EQ(session.selectedClipIds(),
                  (QVariantList{QVariant::fromValue<qulonglong>(a), QVariant::fromValue<qulonglong>(b)}));
        emit session.workspaceChanged();
        EXPECT_EQ(exporter.state(), EditExportAdapter::Review);
        EXPECT_EQ(exporter.reviewOutcome(), review);
    } else {
        if (scenario == "cleared")
            session.clearSelection();
        else
            session.selectClip(b);
        EXPECT_EQ(exporter.state(), scenario == "entire" ? EditExportAdapter::Review : EditExportAdapter::Options);
        if (scenario == "entire")
            EXPECT_EQ(exporter.reviewOutcome(), review);
    }
    EXPECT_EQ(session.workspace().revision(), revision);
    EXPECT_TRUE(QDir(output.path()).entryList(QDir::Files).isEmpty());
}

INSTANTIATE_TEST_SUITE_P(SelectionScope, EditExportReviewSelection,
                         ::testing::Values("changed", "cleared", "confirmation", "entire", "reordered"),
                         [](const ::testing::TestParamInfo<const char*>& info) { return std::string(info.param); });
} // namespace
} // namespace exosnap::quick
