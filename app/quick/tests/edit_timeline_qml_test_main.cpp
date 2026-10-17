// Test runner for the Edit surface's keyboard contract (QCR-504).
//
// A separate executable from record_controls_qml_tests because EditTimeline.qml
// takes three REQUIRED, C++-typed adapters. They cannot be stubbed from QML and
// they cannot be seeded from QML either — `setEditContext` is a plain C++ entry
// point, not Q_INVOKABLE, and making it one to suit a test would widen the
// production API. So the adapters are built here, seeded with the same
// no-master-path fixture context `test_edit_adapters.cpp` uses (nothing is
// opened, decoded or remuxed), and handed to QML as context properties.

#include "EditExportAdapter.h"
#include "EditPlayerAdapter.h"
#include "EditPlayerWorker.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"
#include "ExoEditPlayerItem.h"
#include "RecordViewModelAdapter.h"

#include "viewmodels/RecordViewModel.h"

#include <algorithm>
#include <cstdlib>
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_player_engine.h>
#include <filesystem>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMap>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QUrl>
#include <QtQuickTest>

namespace exosnap::quick {
class EditPlayerAdapterTestPeer {
  public:
    static EditPlayerWorker* worker(EditPlayerAdapter& adapter) {
        return adapter.worker_;
    }
    static uint64_t generation(const EditPlayerAdapter& adapter) {
        return adapter.request_generation_;
    }
    static bool dispatchActive(const EditPlayerAdapter& adapter) {
        return adapter.seek_dispatch_active_;
    }
    static qint64 pendingSeekUs(const EditPlayerAdapter& adapter) {
        return adapter.pending_seek_ ? adapter.pending_seek_->time_us : -1;
    }
};
} // namespace exosnap::quick

using exosnap::EditContext;
using exosnap::quick::EditPlayerAdapter;
using exosnap::quick::EditSessionAdapter;
using exosnap::quick::EditTimelineAdapter;

namespace {

// 100 s, and no `mkv_master_path`: the session reports a duration and accepts
// trims without a decoder ever being asked for anything.
EditContext FixtureContext() {
    EditContext context;
    context.output_path = QStringLiteral("D:/Recordings/clip.mkv");
    context.duration = QStringLiteral("1:40");
    context.size = QStringLiteral("120 MB");
    context.resolution = QStringLiteral("1920x1080");
    context.fps = QStringLiteral("60 fps CFR");
    context.video_codec = QStringLiteral("AV1 (NVENC)");
    context.audio_codec = QStringLiteral("Opus");
    context.container = QStringLiteral("MKV");
    context.duration_seconds = 100.0;
    return context;
}

} // namespace

class MediaPreviewDriver final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString error MEMBER error_ NOTIFY changed)
    Q_PROPERTY(bool ended MEMBER ended_ NOTIFY changed)
  public:
    explicit MediaPreviewDriver(QObject* parent) : QObject(parent) {
        sink_->attach(&item_);
        worker_ = new exosnap::quick::EditPlayerWorker(sink_);
        worker_->moveToThread(&thread_);
        connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
        connect(worker_, &exosnap::quick::EditPlayerWorker::openFinished, this, [this](bool ok, const QString& error) {
            if (!ok)
                error_ = error;
            emit changed();
        });
        connect(worker_, &exosnap::quick::EditPlayerWorker::monitoringRejected, this, [this](const QString& error) {
            error_ = error;
            emit changed();
        });
        connect(worker_, &exosnap::quick::EditPlayerWorker::reachedEnd, this, [this] {
            ended_ = true;
            emit changed();
        });
        thread_.start();
    }
    ~MediaPreviewDriver() override {
        QMetaObject::invokeMethod(worker_, &exosnap::quick::EditPlayerWorker::close, Qt::BlockingQueuedConnection);
        thread_.quit();
        thread_.wait();
        sink_->detach(&item_);
    }
    bool available() const {
        return !qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES").isEmpty();
    }
    Q_INVOKABLE void open() {
        exosnap::edit::Workspace workspace;
        for (const auto* name : {"red.mkv", "blue.mkv"}) {
            exosnap::edit::Asset asset;
            asset.path = (qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/" + name).toStdWString();
            asset.duration = 2'000'000;
            exosnap::engine::EditPlayerEngine player;
            std::string error;
            if (!player.Open(std::filesystem::path(asset.path), error)) {
                error_ = QString::fromStdString(error);
                emit changed();
                return;
            }
            const auto metadata = player.AudioMetadata();
            exosnap::engine::MonitorSelectionSnapshot selection{
                metadata.selection_media_id, {1}, {}, true, metadata.unverified_asset_binding};
            for (const auto& track : metadata.actual_tracks)
                selection.tracks.push_back({track.track_id, 1.0f, false});
            if (!player.SetMonitorSelection(selection, error)) {
                error_ = QString::fromStdString(error);
                emit changed();
                return;
            }
            asset.monitor_selection = std::move(selection);
            workspace.insert(workspace.addAsset(std::move(asset)));
        }
        if (!workspace.crossfade(workspace.clips()[0].id, workspace.clips()[1].id)) {
            error_ = QStringLiteral("Could not construct Crossfade fixture.");
            emit changed();
            return;
        }
        const auto generation = sink_->invalidate();
        QMetaObject::invokeMethod(worker_, [worker = worker_, workspace, generation] {
            worker->setVolume(0);
            worker->setRequestGeneration(generation);
            worker->setTimeline(workspace);
        });
    }
    Q_INVOKABLE void seek(int ms) {
        (void)item_.takePendingFrame();
        last_frame_.clear();
        const auto generation = sink_->invalidate();
        QMetaObject::invokeMethod(worker_, [worker = worker_, ms, generation] {
            worker->setRequestGeneration(generation);
            worker->seek(ms);
        });
    }
    Q_INVOKABLE void play() {
        QMetaObject::invokeMethod(worker_, [worker = worker_] { worker->play(0); });
    }
    Q_INVOKABLE QVariantMap takeFrame() {
        const auto pending = item_.takePendingFrame();
        QVariantMap result;
        if (!pending.timeline)
            return last_frame_;
        const auto& frame = *pending.timeline;
        result["time"] = static_cast<qlonglong>(frame.time_us / 1000);
        result["generation"] = static_cast<qulonglong>(pending.generation);
        result["layers"] = static_cast<int>(frame.layers.size());
        if (!frame.layers.empty()) {
            result["weight"] = frame.layers[0].weight;
            result["source"] = static_cast<qlonglong>(frame.layers[0].frame.pts_us);
        }
        last_frame_ = result;
        return result;
    }
    Q_INVOKABLE bool rejectsStaleFrame() {
        const auto generation = sink_->invalidate();
        (void)item_.takePendingFrame();
        sink_->deliverTimeline({}, generation - 1);
        return !item_.takePendingFrame().timeline.has_value();
    }
    Q_INVOKABLE QVariantMap rawGenerationProbe() {
        exosnap::quick::ExoEditPlayerItem item;
        exosnap::quick::EditPlayerFrameSink sink;
        sink.attach(&item);
        const auto generation = sink.invalidate();
        (void)item.takePendingFrame();
        exosnap::engine::RawDecodedVideoFrame frame;
        frame.pts_us = 33'367;
        sink.deliver(frame, generation - 1, 1'000'000);
        const bool stale_rejected = !item.takePendingFrame().frame.has_value();
        sink.deliver(frame, generation, 1'000'000);
        const auto accepted = item.takePendingFrame();
        return {{"staleRejected", stale_rejected},
                {"sourceTime", accepted.frame ? accepted.frame->pts_us : -1},
                {"timelineTime", accepted.timeline_time_us.value_or(-1)}};
    }
    Q_INVOKABLE QVariantMap supersedeProbe() {
        exosnap::quick::ExoEditPlayerItem item;
        exosnap::quick::EditPlayerFrameSink sink;
        sink.attach(&item);
        const auto generation = sink.invalidate();
        (void)item.takePendingFrame();
        exosnap::engine::RawDecodedVideoFrame frame;
        frame.pts_us = 33'367;
        sink.deliver(frame, generation, 1'000'000);
        const auto accepted = item.takePendingFrame();
        const bool unavailable_before_render = !item.presentedTimeUs().isValid();
        item.publishRenderStateFromRenderThread(accepted.generation, true, QSize(2, 2), {}, accepted.timeline_time_us);
        QCoreApplication::processEvents();
        const bool had_frame = item.hasFrame();
        const auto shown_time = item.presentedTimeUs();
        sink.invalidate();
        const auto superseded = item.takePendingFrame();
        item.publishRenderStateFromRenderThread(accepted.generation, false, {}, {});
        QCoreApplication::processEvents();
        return {{"unavailableBeforeRender", unavailable_before_render},
                {"hadFrame", had_frame},
                {"hasFrame", item.hasFrame()},
                {"shownTime", shown_time},
                {"retainedTime", item.presentedTimeUs()},
                {"clearRequested", superseded.clear}};
    }
    Q_INVOKABLE QVariantMap previewProjectionProbe() {
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setSession(&session);
        const bool initially_empty = player.previewState() == EditPlayerAdapter::Empty;
        session.setEditContext(FixtureContext());
        session.requestSeekUs(33'367);
        return {{"initiallyEmpty", initially_empty},
                {"requestedTime", player.requestedTimeUs()},
                {"hasPresentedTime", player.presentedTimeUs().isValid()},
                {"seekPending", player.seekPending()},
                {"buffering", player.buffering()},
                {"hasDropMeasurement", player.actuallyDroppedRecent().isValid()}};
    }
    Q_INVOKABLE QVariantMap degradedOpenProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setSession(&session);
        session.setEditContext(FixtureContext());
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        emit worker->openFinishedForGeneration(EditPlayerAdapterTestPeer::generation(player) + 1, true,
                                               QStringLiteral("Media unavailable"));
        const bool stale_ignored = player.placeholderText() == QStringLiteral("Preview unavailable");
        emit worker->openFinishedForGeneration(EditPlayerAdapterTestPeer::generation(player), true,
                                               QStringLiteral("Media unavailable"));
        return {{"staleIgnored", stale_ignored},
                {"clipOpen", player.clipOpen()},
                {"failed", player.previewState() == EditPlayerAdapter::Failed},
                {"placeholder", player.placeholderText()}};
    }
    // A seek into blank timeline time presents a black frame for that time and
    // converges; a seek onto unavailable media stays a distinct degraded state.
    Q_INVOKABLE QVariantMap gapSeekProbe() {
        EditSessionAdapter session;
        EditPlayerAdapter player;
        exosnap::quick::ExoEditPlayerItem item;
        player.setSession(&session);
        item.setPlayerAdapter(&player);
        session.setEditContext(FixtureContext());
        auto later = FixtureContext();
        later.duration_seconds = 5.0;
        session.setEditContext(later, 120'000);
        const auto settle = [&](qint64 time_us) {
            QElapsedTimer deadline;
            deadline.start();
            while (deadline.elapsed() < 5'000) {
                auto pending = item.takePendingFrame();
                if (pending.timeline && pending.timeline->time_us == time_us) {
                    item.publishRenderStateFromRenderThread(pending.generation, true, QSize(2, 2), {},
                                                            pending.timeline_time_us);
                    QCoreApplication::processEvents();
                    return QVariantMap{{"layers", static_cast<int>(pending.timeline->layers.size())},
                                       {"width", static_cast<int>(pending.timeline->output.width)}};
                }
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            }
            return QVariantMap{};
        };
        session.requestSeekUs(110'000'000);
        const auto blank = settle(110'000'000);
        QVariantMap result{{"blankLayers", blank.value("layers", -1)},
                           {"blankWidth", blank.value("width", 0)},
                           {"ready", player.previewState() == EditPlayerAdapter::Ready},
                           {"seekPending", player.seekPending()},
                           {"presented", player.presentedTimeUs()},
                           {"requested", player.requestedTimeUs()}};
        session.requestSeekUs(50'000'000);
        QElapsedTimer deadline;
        deadline.start();
        while (player.previewState() != EditPlayerAdapter::Degraded &&
               player.previewState() != EditPlayerAdapter::Failed && deadline.elapsed() < 5'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        result["missingUnavailable"] =
            player.previewState() == EditPlayerAdapter::Degraded || player.previewState() == EditPlayerAdapter::Failed;
        result["missingPlaceholder"] = player.placeholderText();
        item.setPlayerAdapter(nullptr);
        return result;
    }
    Q_INVOKABLE QVariantMap seekCoalescingProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setSession(&session);
        session.setEditContext(FixtureContext());
        QSemaphore entered;
        QSemaphore release;
        QMetaObject::invokeMethod(
            EditPlayerAdapterTestPeer::worker(player),
            [&entered, &release]() {
                entered.release();
                release.acquire();
            },
            Qt::QueuedConnection);
        entered.acquire();
        for (const qint64 at : {1'000, 2'000, 3'000, 4'000})
            session.requestSeekUs(at);
        const bool held_active = EditPlayerAdapterTestPeer::dispatchActive(player);
        const auto held_pending = EditPlayerAdapterTestPeer::pendingSeekUs(player);
        release.release();
        QElapsedTimer deadline;
        deadline.start();
        while ((EditPlayerAdapterTestPeer::dispatchActive(player) ||
                EditPlayerAdapterTestPeer::pendingSeekUs(player) >= 0) &&
               deadline.elapsed() < 5'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return {{"heldActive", held_active},
                {"heldPending", held_pending},
                {"drained", !EditPlayerAdapterTestPeer::dispatchActive(player) &&
                                EditPlayerAdapterTestPeer::pendingSeekUs(player) < 0},
                {"requested", player.requestedTimeUs()}};
    }
    Q_INVOKABLE QVariantMap timelineBurstProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setSession(&session);
        session.setEditContext(FixtureContext());
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        QVariantMap before;
        QMetaObject::invokeMethod(
            worker, [&]() { before = worker->performanceSnapshot(); }, Qt::BlockingQueuedConnection);
        QSemaphore entered;
        QSemaphore release;
        QMetaObject::invokeMethod(
            worker,
            [&]() {
                entered.release();
                release.acquire();
            },
            Qt::QueuedConnection);
        entered.acquire();
        session.selectClip(session.workspace().clips().front().id);
        const auto revision = session.workspace().revision();
        for (int i = 0; i < 12; ++i)
            session.nudgeSelected(1);
        release.release();
        QVariantMap after;
        QMetaObject::invokeMethod(
            worker, [&]() { after = worker->performanceSnapshot(); }, Qt::BlockingQueuedConnection);
        return {
            {"revisions", QVariant::fromValue(session.workspace().revision() - revision)},
            {"updates", after.value("timelineUpdates").toULongLong() - before.value("timelineUpdates").toULongLong()}};
    }
    Q_INVOKABLE QVariantMap splitSourceReuseProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter session;
        EditPlayerAdapter player;
        exosnap::quick::ExoEditPlayerItem item;
        player.setVolume(0);
        player.setSession(&session);
        session.importMedia(QUrl::fromLocalFile(qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/red.mkv"),
                            true, 0);
        QElapsedTimer deadline;
        deadline.start();
        while (!session.open() && deadline.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!session.open())
            return {{"imported", false}};
        item.setPlayerAdapter(&player);
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        qint64 source_time = -1;
        int monitoring_rejections = 0;
        connect(worker, &exosnap::quick::EditPlayerWorker::monitoringRejected, &player,
                [&](const QString&) { ++monitoring_rejections; });
        const auto seek = [&](qint64 target) {
            (void)item.takePendingFrame();
            session.requestSeekUs(target);
            deadline.restart();
            while (deadline.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                const auto frame = item.takePendingFrame();
                if (frame.frame && frame.timeline_time_us && std::abs(*frame.timeline_time_us - target) < 50000) {
                    source_time = frame.frame->pts_us;
                    return true;
                }
            }
            return false;
        };
        const bool initial = seek(250000);
        const bool monitoring_available = !player.monitoringTracks().empty();
        if (monitoring_available) {
            const auto id = player.monitoringTracks().front().toMap().value(QStringLiteral("id")).toString();
            player.confirmMonitoring({id}, true);
        }
        deadline.restart();
        while (player.monitoringConfirmationRequired() && deadline.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const bool monitoring_confirmed = monitoring_available && !player.monitoringConfirmationRequired();
        QVariantMap before;
        QMetaObject::invokeMethod(
            worker, [&]() { before = worker->performanceSnapshot(); }, Qt::BlockingQueuedConnection);
        const auto first = session.workspace().clips().front().id;
        session.selectClip(first);
        session.requestSeekUs(1000000);
        session.splitSelected();
        const auto clips = session.workspace().clips();
        const auto suffix =
            std::find_if(clips.begin(), clips.end(), [](const auto& clip) { return clip.start == 1000000; });
        if (suffix == clips.end()) {
            item.setPlayerAdapter(nullptr);
            return {{"imported", true}, {"split", false}};
        }
        const bool before_move = seek(250000);
        session.moveClip(suffix->id, 3000, false);
        const bool mapped = seek(3500000);
        const qint64 mapped_source_time = source_time;
        const bool back = seek(250000);
        QVariantMap after;
        QMetaObject::invokeMethod(
            worker, [&]() { after = worker->performanceSnapshot(); }, Qt::BlockingQueuedConnection);
        item.setPlayerAdapter(nullptr);
        return {{"imported", true},
                {"split", true},
                {"initial", initial},
                {"beforeMove", before_move},
                {"mapped", mapped},
                {"mappedSourceUs", mapped_source_time},
                {"back", back},
                {"backSourceUs", source_time},
                {"monitoringConfirmed", monitoring_confirmed},
                {"monitoringPreserved", !player.monitoringConfirmationRequired()},
                {"monitoringRejections", monitoring_rejections},
                {"opens", after.value("sessionOpens").toULongLong() - before.value("sessionOpens").toULongLong()}};
    }
    Q_INVOKABLE QVariantMap timelineSeekOrderingProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setSession(&session);
        session.setEditContext(FixtureContext());
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        QSemaphore entered;
        QSemaphore release;
        QMetaObject::invokeMethod(
            worker,
            [&]() {
                entered.release();
                release.acquire();
            },
            Qt::QueuedConnection);
        entered.acquire();
        session.selectClip(session.workspace().clips().front().id);
        session.nudgeSelected(1);
        const auto revision_a = session.workspace().revision();
        session.requestSeekUs(250000);
        QVariantMap at_seek;
        QMetaObject::invokeMethod(worker, [&]() { at_seek = worker->performanceSnapshot(); }, Qt::QueuedConnection);
        session.nudgeSelected(1);
        const auto revision_b = session.workspace().revision();
        release.release();
        QVariantMap final;
        QMetaObject::invokeMethod(
            worker, [&]() { final = worker->performanceSnapshot(); }, Qt::BlockingQueuedConnection);
        return {{"revisionA", QVariant::fromValue(revision_a)},
                {"revisionB", QVariant::fromValue(revision_b)},
                {"atSeekRevision", at_seek.value("timelineRevision")},
                {"finalRevision", final.value("timelineRevision")}};
    }
    Q_INVOKABLE QVariantMap playbackModeChangesProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter session;
        EditPlayerAdapter player;
        exosnap::quick::ExoEditPlayerItem item;
        player.setVolume(0);
        player.setSession(&session);
        const auto fixture = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
        session.importMediaBatch(
            {QUrl::fromLocalFile(fixture + "/red.mkv"), QUrl::fromLocalFile(fixture + "/blue.mkv")}, true, 0);
        QElapsedTimer deadline;
        deadline.start();
        while ((!session.open() || session.workspace().assets().size() != 2) && deadline.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!session.open() || session.workspace().assets().size() != 2)
            return {{"imported", false}};
        item.setPlayerAdapter(&player);
        const auto seek = [&](qint64 target, bool composed, bool blank = false) {
            (void)item.takePendingFrame();
            session.requestSeekUs(target);
            deadline.restart();
            while (deadline.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                const auto frame = item.takePendingFrame();
                if (!frame.timeline_time_us || std::abs(*frame.timeline_time_us - target) >= 50000)
                    continue;
                if (blank)
                    return frame.timeline && frame.timeline->layers.empty();
                if (composed)
                    return frame.timeline && !frame.timeline->layers.empty() &&
                           frame.timeline->layers.front().frame.width > 0;
                return frame.frame && frame.frame->width > 0;
            }
            return false;
        };
        const auto first = session.workspace().clips().front().id;
        const auto second = std::find_if(session.workspace().clips().begin(), session.workspace().clips().end(),
                                         [](const auto& clip) { return clip.start == 2000000; });
        if (second == session.workspace().clips().end()) {
            item.setPlayerAdapter(nullptr);
            return {{"imported", true}, {"twoClips", false}};
        }
        const auto second_id = second->id;
        const bool initial = seek(250000, false);
        session.setClipFade(first, true, 200);
        const bool composed = seek(100000, true);
        session.setClipFade(first, true, 0);
        const bool raw_restored = seek(250000, false);
        session.moveClip(second_id, 5000, false);
        const bool gap = seek(3000000, true, true);
        const bool after_gap = seek(5500000, false);
        QVariantMap before;
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        QMetaObject::invokeMethod(
            worker, [&]() { before = worker->performanceSnapshot(); }, Qt::BlockingQueuedConnection);
        const bool other_source = seek(250000, false);
        QVariantMap after;
        QMetaObject::invokeMethod(
            worker, [&]() { after = worker->performanceSnapshot(); }, Qt::BlockingQueuedConnection);
        item.setPlayerAdapter(nullptr);
        return {{"imported", true},
                {"twoClips", true},
                {"initial", initial},
                {"composed", composed},
                {"rawRestored", raw_restored},
                {"gap", gap},
                {"afterGap", after_gap},
                {"otherSource", other_source},
                {"otherSourceOpens",
                 after.value("sessionOpens").toULongLong() - before.value("sessionOpens").toULongLong()}};
    }
    Q_INVOKABLE QVariantMap supersededOpenReadinessProbe(bool composed) {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter session;
        EditPlayerAdapter player;
        exosnap::quick::ExoEditPlayerItem item;
        player.setVolume(0);
        player.setSession(&session);
        QSemaphore entered;
        QSemaphore release;
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        QMetaObject::invokeMethod(
            worker,
            [&entered, &release]() {
                entered.release();
                release.acquire();
            },
            Qt::QueuedConnection);
        entered.acquire();
        session.importMedia(QUrl::fromLocalFile(qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/red.mkv"),
                            true, 0);
        QElapsedTimer deadline;
        deadline.start();
        while (!session.open() && deadline.elapsed() < 5'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!session.open()) {
            release.release();
            return {{"imported", false}, {"error", session.workspaceError()}};
        }
        if (composed)
            session.setClipFade(session.workspace().clips().front().id, true, 200);
        const auto open_generation = EditPlayerAdapterTestPeer::generation(player);
        bool superseded_open = false;
        connect(worker, &exosnap::quick::EditPlayerWorker::openFinishedForGeneration, &player,
                [&player, &superseded_open](qulonglong generation, bool opened, const QString&) {
                    if (opened && generation != EditPlayerAdapterTestPeer::generation(player))
                        superseded_open = true;
                });
        item.setPlayerAdapter(&player);
        session.requestSeekUs(500'000);
        const auto seek_generation = EditPlayerAdapterTestPeer::generation(player);
        release.release();
        qint64 decoded_time = -1;
        const auto observe = [&]() {
            const auto pending = item.takePendingFrame();
            if ((pending.frame && pending.frame->width > 0) || (pending.timeline && !pending.timeline->layers.empty() &&
                                                                pending.timeline->layers.front().frame.width > 0)) {
                decoded_time = pending.timeline_time_us.value_or(-1);
                return true;
            }
            return false;
        };
        deadline.restart();
        bool decoded = false;
        while (!decoded && deadline.elapsed() < 5'000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            decoded = observe() && std::abs(decoded_time - 500'000) <= 100'000;
        }
        deadline.restart();
        while ((EditPlayerAdapterTestPeer::dispatchActive(player) ||
                EditPlayerAdapterTestPeer::pendingSeekUs(player) >= 0) &&
               deadline.elapsed() < 5'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const bool clip_open = player.clipOpen();
        int playback_frames = 0;
        bool advanced = false;
        if (clip_open) {
            bool monitoring_requested = false;
            player.setPlaying(true);
            deadline.restart();
            while (!advanced && deadline.elapsed() < 5'000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                if (player.monitoringConfirmationRequired() && !monitoring_requested &&
                    !player.monitoringTracks().empty()) {
                    const auto id = player.monitoringTracks().front().toMap().value(QStringLiteral("id")).toString();
                    player.confirmMonitoring({id}, true);
                    monitoring_requested = true;
                }
                if (monitoring_requested && !player.monitoringConfirmationRequired() && !player.playing())
                    player.setPlaying(true);
                if (observe())
                    ++playback_frames;
                advanced = player.playing() && session.positionUs() >= 750'000 && playback_frames >= 3;
            }
            player.setPlaying(false);
        }
        const bool presented = player.presentedTimeUs().isValid();
        item.setPlayerAdapter(nullptr);
        return {{"imported", true},
                {"supersededOpen", superseded_open},
                {"generationSuperseded", seek_generation > open_generation},
                {"decoded", decoded},
                {"clipOpen", clip_open},
                {"playbackAdvanced", advanced},
                {"playbackFrames", playback_frames},
                {"hasPresentedTime", presented},
                {"volume", player.volume()},
                {"positionUs", session.positionUs()},
                {"monitoringRequired", player.monitoringConfirmationRequired()},
                {"placeholder", player.placeholderText()}};
    }
    Q_INVOKABLE QVariantMap thumbnailEditReuseProbe() {
        QTemporaryDir directory;
        EditSessionAdapter session;
        const auto fixture = qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES");
        session.importMediaBatch(
            {QUrl::fromLocalFile(fixture + "/red.mkv"), QUrl::fromLocalFile(fixture + "/blue.mkv")}, true, 0);
        QElapsedTimer deadline;
        deadline.start();
        while (session.media().size() < 2 && deadline.elapsed() < 5'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!directory.isValid() || session.media().size() != 2)
            return {{"imported", false}, {"error", session.workspaceError()}};
        exosnap::ThumbnailRequestBroker::Config config;
        config.disk_root = std::filesystem::path(directory.path().toStdWString());
        QMap<QString, QImage> reference;
        QStringList identities;
        const auto register_rows = [&](EditTimelineAdapter& timeline) {
            bool registered = true;
            for (const auto& value : session.visibleClips(0, 10'000)) {
                const auto row = value.toMap();
                if (row.value("video").toBool()) {
                    const auto identity = row.value("sourceIdentity").toString();
                    if (!identities.contains(identity))
                        identities.append(identity);
                    registered = timeline.registerThumbnailSource(
                                     row.value("sourceIdentity").toString(), row.value("path").toString(),
                                     row.value("width").toInt(), row.value("height").toInt(),
                                     row.value("fpsNum").toInt(), row.value("fpsDen").toInt()) &&
                                 registered;
                }
            }
            return registered;
        };
        // The first failing request, so a failure names its phase and cause instead of
        // only cascading into later phases that compare against a missing reference.
        QString failure;
        const auto fail = [&failure](const QString& phase, int tile, const QString& reason) {
            if (failure.isEmpty())
                failure = QStringLiteral("%1 tile %2: %3").arg(phase).arg(tile).arg(reason);
            return false;
        };
        const auto request_rows = [&](EditTimelineAdapter& timeline,
                                      exosnap::quick::EditThumbnailImageProvider& provider, const QString& phase) {
            const bool cold = phase == QLatin1String("initial");
            int requested = 0;
            for (const auto& value : session.visibleClips(0, 10'000)) {
                const auto row = value.toMap();
                if (!row.value("video").toBool())
                    continue;
                const auto in = row.value("sourceInUs").toLongLong();
                const auto out = row.value("sourceOutUs").toLongLong();
                const auto tiles = timeline.thumbnailTiles(row.value("sourceIdentity").toString(), in, out,
                                                           (out - in) * 96.0 / 1'000'000, 48);
                if (tiles.empty())
                    return fail(phase, requested, QStringLiteral("no tiles for a registered source"));
                for (const auto& tile : tiles) {
                    const auto url = tile.toMap().value("source").toString();
                    std::unique_ptr<QQuickImageResponse> response(
                        provider.requestImageResponse(QUrl(url).path().mid(1), {}));
                    bool finished = false;
                    connect(response.get(), &QQuickImageResponse::finished, response.get(),
                            [&finished]() { finished = true; });
                    deadline.restart();
                    while (!finished && deadline.elapsed() < 5'000)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    if (!finished)
                        return fail(phase, requested,
                                    QStringLiteral("no response within %1 ms").arg(deadline.elapsed()));
                    if (!response->errorString().isEmpty())
                        return fail(phase, requested, response->errorString());
                    std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
                    const auto image = texture ? texture->image().convertToFormat(QImage::Format_RGBA8888) : QImage();
                    if (image.isNull() || image.height() != 48)
                        return fail(phase, requested,
                                    QStringLiteral("image %1x%2").arg(image.width()).arg(image.height()));
                    if (cold)
                        reference.insert(url, image);
                    else if (!reference.contains(url))
                        return fail(phase, requested, QStringLiteral("no cold reference for %1").arg(url));
                    else if (reference.value(url) != image)
                        return fail(phase, requested, QStringLiteral("pixels differ from the cold reference"));
                    ++requested;
                }
            }
            return requested > 0;
        };
        bool initial = false;
        bool trimmed = false;
        bool split = false;
        bool moved = false;
        qulonglong cold_decodes = 0;
        qulonglong warm_decodes = 0;
        {
            EditTimelineAdapter timeline;
            timeline.configureThumbnailCache(config);
            timeline.setSession(&session);
            std::unique_ptr<exosnap::quick::EditThumbnailImageProvider> provider(
                timeline.createThumbnailImageProvider());
            initial = register_rows(timeline) && request_rows(timeline, *provider, QStringLiteral("initial"));
            timeline.setForegroundBusy(true);
            QElapsedTimer stable;
            stable.start();
            deadline.restart();
            auto observed = timeline.thumbnailBroker()->Stats().decodes;
            while (stable.elapsed() < 100 && deadline.elapsed() < 5'000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                const auto count = timeline.thumbnailBroker()->Stats().decodes;
                if (count != observed || timeline.thumbnailBroker()->HasWork()) {
                    observed = count;
                    stable.restart();
                }
            }
            cold_decodes = timeline.thumbnailBroker()->Stats().decodes;
            const auto red = session.visibleClips(0, 1'000).front().toMap();
            session.trimClip(red.value("id").toULongLong(), 500, 2000);
            trimmed = session.workspaceError().isEmpty() &&
                      session.workspace().clip(red.value("id").toULongLong())->source_in == 500'000 &&
                      request_rows(timeline, *provider, QStringLiteral("trimmed"));
            session.selectClip(red.value("id").toULongLong());
            session.requestSeek(1000);
            session.splitSelected();
            split =
                session.workspace().clips().size() == 6 && request_rows(timeline, *provider, QStringLiteral("split"));
            for (const auto& value : session.visibleClips(0, 10'000)) {
                const auto row = value.toMap();
                if (row.value("video").toBool() && row.value("sourceIdentity") == red.value("sourceIdentity") &&
                    row.value("sourceInUs").toLongLong() == 1'000'000) {
                    session.moveClip(row.value("id").toULongLong(), 5000, false);
                    moved = session.workspaceError().isEmpty() &&
                            session.workspace().clip(row.value("id").toULongLong())->start == 5'000'000 &&
                            request_rows(timeline, *provider, QStringLiteral("moved"));
                    break;
                }
            }
            warm_decodes = timeline.thumbnailBroker()->Stats().decodes;
        }
        EditTimelineAdapter reopened;
        reopened.configureThumbnailCache(config);
        reopened.setSession(&session);
        reopened.setForegroundBusy(true);
        std::unique_ptr<exosnap::quick::EditThumbnailImageProvider> provider(reopened.createThumbnailImageProvider());
        const bool disk_reused =
            register_rows(reopened) && request_rows(reopened, *provider, QStringLiteral("reopened"));
        const auto stats = reopened.thumbnailBroker()->Stats();
        return {{"imported", true},
                {"initial", initial},
                {"trimmed", trimmed},
                {"split", split},
                {"moved", moved},
                {"coldDecodes", cold_decodes},
                {"warmDecodes", warm_decodes},
                {"diskReused", disk_reused},
                {"reopenDecodes", QVariant::fromValue<qulonglong>(stats.decodes)},
                {"diskHits", QVariant::fromValue<qulonglong>(stats.disk_hits)},
                {"sourceKeys", reference.size()},
                {"sources", identities.size()},
                {"failure", failure}};
    }
  signals:
    void changed();

  private:
    std::shared_ptr<exosnap::quick::EditPlayerFrameSink> sink_ =
        std::make_shared<exosnap::quick::EditPlayerFrameSink>();
    exosnap::quick::ExoEditPlayerItem item_;
    QThread thread_;
    exosnap::quick::EditPlayerWorker* worker_ = nullptr;
    QString error_;
    QVariantMap last_frame_;
    bool ended_ = false;
};

class Setup final : public QObject {
    Q_OBJECT

  public slots:
    QVariantMap timelineCheckpoint() const {
        return {{"revision", QVariant::fromValue<qulonglong>(session_->workspace().revision())},
                {"historyBytes", QVariant::fromValue<qulonglong>(session_->workspace().historyBytes())}};
    }
    void exportVisualState(int state) {
        page_exporter_->applyVisualState(static_cast<exosnap::quick::EditExportAdapter::State>(state), 25, {}, {});
    }
    // What the player worker publishes after opening a source with audio.
    void pageMonitoring(const QVariantList& tracks, bool confirmation_required) {
        page_player_->setMonitoringState(tracks, confirmation_required);
    }
    void applicationAvailable() {
        QCoreApplication::setApplicationName(QStringLiteral("edit-timeline-qml-tests"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("testPersistenceStage"),
                                                  qEnvironmentVariable("EXOSNAP_EDIT_PERSISTENCE_STAGE"));
        engine->rootContext()->setContextProperty(QStringLiteral("testVisualDirectory"),
                                                  qEnvironmentVariable("EXOSNAP_EDIT_VISUAL_DIR"));
        engine->rootContext()->setContextProperty(QStringLiteral("testScale"),
                                                  qEnvironmentVariable("QT_SCALE_FACTOR", "1"));
        engine->rootContext()->setContextProperty(QStringLiteral("testMediaPreview"), new MediaPreviewDriver(this));
        session_ = new EditSessionAdapter(this);
        timeline_ = new EditTimelineAdapter(this);
        player_ = new EditPlayerAdapter(this);

        session_->setEditContext(FixtureContext());
        // A SECOND player, seeded with an open clip. The timeline's own cases
        // assert what the keyboard does when nothing is open, so the transport
        // cases -- which need the opposite -- cannot share that adapter.
        // Nothing decodes either way: the fixture carries no master path.
        transport_player_ = new EditPlayerAdapter(this);
        transport_player_->setClipStateForTest(/*clip_open=*/true, /*duration_ms=*/100'000);

        engine->rootContext()->setContextProperty(QStringLiteral("testSession"), session_);
        engine->rootContext()->setContextProperty(QStringLiteral("testTimeline"), timeline_);
        engine->rootContext()->setContextProperty(QStringLiteral("testPlayer"), player_);
        engine->rootContext()->setContextProperty(QStringLiteral("testTransportPlayer"), transport_player_);

        auto* page_session = new EditSessionAdapter(this);
        auto* page_timeline = new EditTimelineAdapter(this);
        auto* page_player = new EditPlayerAdapter(this);
        page_player_ = page_player;
        auto* page_exporter = new exosnap::quick::EditExportAdapter(this);
        page_exporter_ = page_exporter;
        engine->rootContext()->setContextProperty(QStringLiteral("testEditHarness"), this);
        auto* page_recordings = new exosnap::quick::RecordViewModelAdapter(&record_view_model_, this);
        page_exporter->setSession(page_session);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageSession"), page_session);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageTimeline"), page_timeline);
        engine->rootContext()->setContextProperty(QStringLiteral("testPagePlayer"), page_player);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageExporter"), page_exporter);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageRecordings"), page_recordings);
    }

  private:
    exosnap::RecordViewModel record_view_model_;
    EditSessionAdapter* session_ = nullptr;
    EditTimelineAdapter* timeline_ = nullptr;
    EditPlayerAdapter* player_ = nullptr;
    EditPlayerAdapter* transport_player_ = nullptr;
    EditPlayerAdapter* page_player_ = nullptr;
    exosnap::quick::EditExportAdapter* page_exporter_ = nullptr;
};

QUICK_TEST_MAIN_WITH_SETUP(edit_timeline, Setup)

#include "edit_timeline_qml_test_main.moc"
