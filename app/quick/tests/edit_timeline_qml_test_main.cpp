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
#include "EditTransportFixture.h"
#include "ExoEditPlayerItem.h"
#include "QuickThemeTokens.h"
#include "RecordViewModelAdapter.h"
#include "RecordingReviewAdapter.h"
#include "ShellAdapter.h"

#include "viewmodels/RecordViewModel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_player_engine.h>
#include <filesystem>
#include <set>
#include <string>
#include <utility>

#include <QBuffer>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QLocale>
#include <QMap>
#include <QMimeData>
#include <QObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTranslator>
#include <QUrl>
#include <QtQuickTest>

namespace exosnap::quick {
class EditPlayerWorkerTestPeer {
  public:
    static void gateVideo(EditPlayerWorker& worker, std::function<bool(const std::atomic<bool>&)> gate) {
        worker.video_before_read_ = std::move(gate);
    }
    static void gateAudio(EditPlayerWorker& worker, std::function<bool(const std::atomic<bool>&)> gate) {
        worker.audio_before_read_ = std::move(gate);
    }
    static void gateOpen(EditPlayerWorker& worker, std::function<bool(const std::atomic<bool>&)> gate) {
        worker.video_before_open_ = std::move(gate);
    }
    static auto currentProducer(const EditPlayerWorker& worker) {
        return worker.audio_producer_;
    }
    static auto currentAudioOrigin(const EditPlayerWorker& worker) {
        return worker.audio_sample_;
    }
    static void unavailableEndpoint(EditPlayerWorker& worker) {
        worker.audio_output_error_ = QStringLiteral("Injected unavailable component endpoint.");
    }
    static bool paused(const EditPlayerWorker& worker) {
        return !worker.timeline_playing_ && !worker.published_playing_;
    }
    static bool adoptPrepared(EditPlayerWorker& worker) {
        return worker.adoptPreparedRevision();
    }
    static bool mixCurrent(EditPlayerWorker& worker) {
        if (!worker.audio_producer_)
            return false;
        std::array<float, 960> samples{};
        worker.audio_producer_->Mix(worker.audio_stream_generation_, worker.audio_sample_, samples);
        return std::any_of(samples.begin(), samples.end(), [](float sample) { return sample != 0; }) &&
               worker.audio_producer_->Measurements().applied_revision == worker.workspace_->revision();
    }
    static void componentPlaying(EditPlayerWorker& worker) {
        worker.timeline_playing_ = true;
        worker.publishPlaybackState(true);
        worker.elapsed_.restart();
    }
    static std::shared_ptr<engine::TimelineAudioProducer> prepareComponentAudio(EditPlayerWorker& worker) {
        worker.audio_producer_ = std::make_shared<engine::TimelineAudioProducer>();
        const engine::TimelineAudioStamp stamp{worker.audio_session_generation_, worker.workspace_->revision(),
                                               ++worker.audio_stream_generation_};
        worker.audio_producer_->Prepare(std::make_shared<const engine::TimelineRenderSnapshot>(worker.render_snapshot_),
                                        stamp, 0);
        worker.publishAudioControls();
        return worker.audio_producer_;
    }
    static qint64 duration(const EditPlayerWorker& worker) {
        return worker.workspace_->duration();
    }
    static qint64 position(const EditPlayerWorker& worker) {
        return worker.position_us_;
    }
    static bool audio(EditPlayerWorker& worker, int64_t sample, std::vector<float>& output, std::string& error) {
        engine::TimelineAudioProducer producer;
        const engine::TimelineAudioStamp stamp{1, worker.workspace_->revision(), 1};
        auto controls = engine::BuildTimelineAudioControls(worker.workspace_->timeline(), stamp);
        if (!controls.succeeded()) {
            error = controls.error()->context;
            return false;
        }
        producer.Prepare(std::make_shared<const engine::TimelineRenderSnapshot>(worker.render_snapshot_), stamp,
                         sample);
        producer.SetControls(*controls.value());
        QElapsedTimer deadline;
        deadline.start();
        while (producer.Measurements().source_buffer_frames < 4800 && !producer.Measurements().failed &&
               deadline.elapsed() < 5'000)
            QThread::msleep(1);
        if (producer.Measurements().failed || producer.Measurements().source_buffer_frames < 4800) {
            error = producer.Error().empty() ? "The timeline PCM reservoir did not fill." : producer.Error();
            return false;
        }
        output.resize(4800 * 2);
        producer.Mix(1, sample, output);
        return producer.Measurements().underrun_frames == 0;
    }
    static bool composed(const EditPlayerWorker& worker) {
        return worker.render_mode_;
    }
};

class EditPlayerAdapterTestPeer {
  public:
    static bool desiredPlaying(const EditPlayerAdapter& adapter) {
        return adapter.desired_playing_;
    }
    static void acknowledge(EditPlayerAdapter& adapter, bool playing) {
        emit adapter.worker_->transportStateChanged(adapter.transport_command_, adapter.request_generation_, playing);
    }
    static EditPlayerWorker* worker(EditPlayerAdapter& adapter) {
        return adapter.worker_;
    }
    static uint64_t generation(const EditPlayerAdapter& adapter) {
        return adapter.request_generation_;
    }
    static uint64_t command(const EditPlayerAdapter& adapter) {
        return adapter.transport_command_;
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
    Q_INVOKABLE QVariantMap transportIntentProbe(const QString& action) {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setSession(&session);
        session.setEditContext(FixtureContext());
        player.setClipStateForTest(true, 100'000);
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
        player.setPlaying(true);
        const auto placeholder_before = player.placeholderText();
        if (action == QStringLiteral("toggle")) {
            player.togglePlay();
        } else if (action == QStringLiteral("seek")) {
            EditPlayerAdapterTestPeer::acknowledge(player, true);
            session.requestSeekUs(20'000'000);
        } else if (action == QStringLiteral("scrub")) {
            player.beginScrub();
            player.beginScrub();
            player.endScrub();
        } else if (action == QStringLiteral("pauseDuringScrub")) {
            EditPlayerAdapterTestPeer::acknowledge(player, true);
            player.beginScrub();
            player.setPlaying(false);
            player.endScrub();
        } else if (action == QStringLiteral("authoredEdit")) {
            session.setFixedTimelineEnd(true, 110000);
        } else if (action == QStringLiteral("editDuringScrub")) {
            player.beginScrub();
            session.setFixedTimelineEnd(true, 110000);
            player.endScrub();
        } else if (action == QStringLiteral("frameStep")) {
            session.requestFrameStep(1);
        } else if (action == QStringLiteral("oldPosition")) {
            session.requestSeekUs(20'000'000);
            emit worker->positionAdvancedForCommand(EditPlayerAdapterTestPeer::command(player) - 1,
                                                    EditPlayerAdapterTestPeer::generation(player), 1'000);
        } else if (action == QStringLiteral("oldError")) {
            session.requestSeekUs(20'000'000);
            emit worker->monitoringRejectedForCommand(EditPlayerAdapterTestPeer::command(player),
                                                      EditPlayerAdapterTestPeer::generation(player) - 1,
                                                      QStringLiteral("An obsolete source error"));
        } else if (action == QStringLiteral("oldOpenError")) {
            emit worker->openFinishedForCommand(EditPlayerAdapterTestPeer::command(player) - 1,
                                                EditPlayerAdapterTestPeer::generation(player), false,
                                                QStringLiteral("An obsolete open error"));
        } else if (action == QStringLiteral("oldAcknowledgement")) {
            const auto command = EditPlayerAdapterTestPeer::command(player);
            const auto generation = EditPlayerAdapterTestPeer::generation(player);
            player.setPlaying(false);
            EditPlayerAdapterTestPeer::acknowledge(player, false);
            emit worker->transportStateChanged(command, generation, true);
            emit worker->transportCompleted(command, generation, true);
            emit worker->transportEnded(command, generation);
        } else if (action == QStringLiteral("currentError")) {
            emit worker->monitoringRejectedForCommand(EditPlayerAdapterTestPeer::command(player),
                                                      EditPlayerAdapterTestPeer::generation(player),
                                                      QStringLiteral("A current source error"));
        } else if (action == QStringLiteral("runtimeFailure")) {
            emit worker->openFinishedForCommand(EditPlayerAdapterTestPeer::command(player),
                                                EditPlayerAdapterTestPeer::generation(player), false,
                                                QStringLiteral("The audio output stopped unexpectedly."));
        }
        const bool desired = EditPlayerAdapterTestPeer::desiredPlaying(player);
        release.release();
        return {{"desiredPlaying", desired},
                {"workspaceCopyNs", player.workspaceCopyTimeNs()},
                {"requested", player.requestedTimeUs()},
                {"playhead", session.positionUs()},
                {"acknowledgedPlaying", player.playing()},
                {"placeholderUnchanged", player.placeholderText() == placeholder_before}};
    }
    Q_INVOKABLE QVariantMap blankTransportProbe() {
        namespace edit = exosnap::edit;
        edit::Workspace workspace;
        const auto result =
            workspace.apply(edit::SetTimelineEnd{{edit::TimelineEndMode::FixedEnd, 100'000'000}}, workspace.revision());
        exosnap::quick::EditPlayerWorker worker(std::make_shared<exosnap::quick::EditPlayerFrameSink>());
        bool playing = false;
        connect(&worker, &exosnap::quick::EditPlayerWorker::playbackStateChanged, &worker,
                [&](bool state) { playing = state; });
        worker.setTimeline(std::move(workspace));
        worker.requestTransport(1, 1, 10'000'000, false, true);
        const bool started = playing;
        worker.requestTransport(2, 2, 20'000'000, true, true);
        const bool continued = playing;
        worker.requestTransport(3, 3, 30'000'000, true, false);
        return {{"authored", result.status == edit::CommandStatus::Applied},
                {"started", started},
                {"continued", continued},
                {"paused", !playing}};
    }
    Q_INVOKABLE QVariantMap authoredAudioAdmissionProbe(bool composed, const QString& filename) {
        EditSessionAdapter session;
        EditPlayerAdapter player;
        exosnap::quick::ExoEditPlayerItem item;
        item.setPlayerAdapter(&player);
        player.setVolume(0);
        player.setSession(&session);
        session.importMedia(QUrl::fromLocalFile(fixturePath(filename)), true, 0);
        QElapsedTimer deadline;
        deadline.start();
        while (!session.open() && deadline.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!session.open())
            return {{"imported", false}, {"error", session.workspaceError()}};
        if (composed)
            session.setClipFade(session.workspace().clips().front().id, true, 200);
        session.selectClip(0);
        player.setPlaying(true);
        const bool intent_retained = player.desiredPlaying();
        int frames = 0;
        bool saw_video = false;
        deadline.restart();
        while ((session.positionUs() < 50'000 || frames < 2) && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            const auto frame = item.takePendingFrame();
            if (frame.frame || frame.timeline)
                ++frames;
            saw_video = saw_video || frame.frame || (frame.timeline && !frame.timeline->layers.empty());
        }
        const bool advanced = session.positionUs() >= 50'000;
        session.requestSeekUs(500'000);
        const bool preserved_intent = player.desiredPlaying();
        deadline.restart();
        while (session.positionUs() < 550'000 && deadline.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const bool seek_continued = preserved_intent && player.playing() && session.positionUs() >= 550'000;
        player.setPlaying(false);
        session.requestSeekUs(750'000);
        deadline.restart();
        while ((player.transportPending() || player.playing()) && deadline.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const bool seek_paused = !player.desiredPlaying() && !player.playing() && session.positionUs() == 750'000;
        session.requestSeekUs(session.workspace().duration());
        player.setPlaying(true);
        deadline.restart();
        while ((!player.playing() || session.positionUs() >= 250'000) && deadline.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const bool restarted = player.playing() && session.positionUs() < 250'000;
        player.setPlaying(false);
        item.setPlayerAdapter(nullptr);
        const auto& clips = session.workspace().clips();
        const auto enabled_audio = std::count_if(clips.begin(), clips.end(), [](const auto& clip) {
            return clip.enabled && clip.stream && clip.stream->kind == exosnap::engine::TimelineStreamKind::Audio;
        });
        return {{"imported", true},
                {"intentRetained", intent_retained},
                {"advanced", advanced},
                {"selectionIndependent", session.workspace().selection() == 0},
                {"playingSeekContinued", seek_continued},
                {"pausedSeekStayedPaused", seek_paused},
                {"restartedAtEnd", restarted},
                {"frames", frames},
                {"sawVideo", saw_video},
                {"enabledAudio", static_cast<int>(enabled_audio)},
                {"audioChoiceRequired", session.workspace().assets().front().mix_decision_required},
                {"error", player.placeholderText()}};
    }
    // The ambiguous-transport source is generated in-process; every other file
    // comes from the externally provided render fixtures.
    static QString fixturePath(const QString& filename) {
        if (filename == QStringLiteral("transport-ambiguous.mkv"))
            return exosnap::quick::test::TransportAmbiguousFixture();
        return qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/" + filename;
    }
    Q_INVOKABLE bool fixtureAvailable(const QString& filename) const {
        std::error_code error;
        return std::filesystem::is_regular_file(std::filesystem::path(fixturePath(filename).toStdWString()), error);
    }
    Q_INVOKABLE QVariantMap transportBurstProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditPlayerAdapter player;
        player.setClipStateForTest(true, 100'000);
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        QSemaphore entered;
        QSemaphore release;
        std::atomic<int> completed = 0;
        connect(
            worker, &exosnap::quick::EditPlayerWorker::transportCompleted, worker,
            [&](qulonglong, qulonglong, bool) { ++completed; }, Qt::DirectConnection);
        QMetaObject::invokeMethod(
            worker,
            [&]() {
                entered.release();
                release.acquire();
            },
            Qt::QueuedConnection);
        entered.acquire();
        for (int index = 0; index < 5000; ++index)
            player.togglePlay();
        release.release();
        QMetaObject::invokeMethod(worker, []() {}, Qt::BlockingQueuedConnection);
        return {{"completed", completed.load()}, {"desiredPlaying", player.desiredPlaying()}};
    }
    Q_INVOKABLE QVariantMap sessionReplacementProbe() {
        EditSessionAdapter first;
        auto second = std::make_unique<EditSessionAdapter>();
        EditPlayerAdapter player;
        auto* worker = exosnap::quick::EditPlayerAdapterTestPeer::worker(player);
        player.setSession(&first);
        first.setEditContext(FixtureContext());
        QMetaObject::invokeMethod(worker, []() {}, Qt::BlockingQueuedConnection);
        player.setSession(second.get());
        auto context = FixtureContext();
        context.duration_seconds = 5.0;
        second->setEditContext(context);
        qint64 duration = 0;
        QMetaObject::invokeMethod(
            worker, [&]() { duration = exosnap::quick::EditPlayerWorkerTestPeer::duration(*worker); },
            Qt::BlockingQueuedConnection);
        player.setClipStateForTest(true, 5000);
        second.reset();
        return {{"duration", duration}, {"closedAfterDestroy", !player.clipOpen()}};
    }
    Q_INVOKABLE QVariantMap pauseBeforePresentationProbe() {
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setSession(&session);
        session.setFixedTimelineEnd(true, 100'000);
        player.setPlaying(true);
        QElapsedTimer deadline;
        deadline.start();
        while (session.positionUs() < 50'000 && deadline.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const auto pause_at = session.positionUs();
        const bool presentation_pending = player.seekPending();
        player.setPlaying(false);
        auto* worker = exosnap::quick::EditPlayerAdapterTestPeer::worker(player);
        qint64 worker_at = -1;
        QMetaObject::invokeMethod(
            worker, [&]() { worker_at = exosnap::quick::EditPlayerWorkerTestPeer::position(*worker); },
            Qt::BlockingQueuedConnection);
        return {{"started", pause_at >= 50'000},
                {"presentationPending", presentation_pending},
                {"pauseAt", pause_at},
                {"workerAt", worker_at}};
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
        emit worker->openFinishedForCommand(EditPlayerAdapterTestPeer::command(player),
                                            EditPlayerAdapterTestPeer::generation(player) + 1, true,
                                            QStringLiteral("Media unavailable"));
        const bool stale_ignored = player.placeholderText() == QStringLiteral("Preview unavailable");
        emit worker->openFinishedForCommand(EditPlayerAdapterTestPeer::command(player),
                                            EditPlayerAdapterTestPeer::generation(player), true,
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
            session.nudgeSelected(17);
        release.release();
        QVariantMap after;
        QMetaObject::invokeMethod(
            worker, [&]() { after = worker->performanceSnapshot(); }, Qt::BlockingQueuedConnection);
        return {
            {"revisions", QVariant::fromValue(session.workspace().revision() - revision)},
            {"updates", after.value("timelineUpdates").toULongLong() - before.value("timelineUpdates").toULongLong()}};
    }
    Q_INVOKABLE QVariantMap timelineAudioEditsProbe() {
        namespace edit = exosnap::edit;
        edit::Workspace baseline;
        edit::Asset asset;
        asset.path = (qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/red.mkv").toStdWString();
        asset.duration = 2'000'000;
        const auto metadata = exosnap::engine::ProbeEditMedia(asset.path);
        if (!metadata.error.empty() || metadata.audio_metadata.actual_tracks.empty())
            return {{"error", QString::fromStdString(metadata.error)}};
        const auto& audio = metadata.audio_metadata;
        const auto track = audio.actual_tracks.front().track_id;
        asset.monitor_selection = exosnap::engine::MonitorSelectionSnapshot{
            audio.selection_media_id, {1}, {{track, 1.0f, false}}, true, audio.unverified_asset_binding};
        baseline.insert(baseline.addAsset(std::move(asset)));
        const auto audio_clip = std::find_if(baseline.clips().begin(), baseline.clips().end(), [&](const auto& clip) {
            return std::any_of(baseline.tracks().begin(), baseline.tracks().end(), [&](const auto& value) {
                return value.id == clip.track && value.type == edit::TrackType::Audio;
            });
        });
        if (audio_clip == baseline.clips().end())
            return {{"error", QStringLiteral("The fixture has no audio clip.")}};
        const auto id = audio_clip->id;
        exosnap::quick::EditPlayerWorker worker(std::make_shared<exosnap::quick::EditPlayerFrameSink>());
        std::string error;
        bool read_ok = true;
        const auto energy = [&](edit::Workspace workspace, int64_t sample) {
            worker.setTimeline(std::move(workspace));
            std::vector<float> samples;
            read_ok = exosnap::quick::EditPlayerWorkerTestPeer::audio(worker, sample, samples, error) && read_ok;
            double sum = 0;
            for (const auto value : samples)
                sum += value * value;
            return samples.empty() ? 0.0 : std::sqrt(sum / samples.size());
        };
        const double original = energy(baseline, 4800);
        auto edited = baseline;
        read_ok = edited.apply(edit::SetGain{edit::GainTarget::Clip, id, -6.0}, edited.revision()).status ==
                      edit::CommandStatus::Applied &&
                  read_ok;
        const double quieter = energy(edited, 4800);
        edited = baseline;
        edited.apply(edit::SetClipEnabled{id, false}, edited.revision());
        const double muted = energy(edited, 4800);
        edited = baseline;
        edited.apply(edit::LiftClips{{id}, false}, edited.revision());
        const double deleted = energy(edited, 4800);
        edited = baseline;
        edit::Move move{{id}, 500'000};
        move.expand_linked = false;
        edited.apply(move, edited.revision());
        const double before_audio = energy(edited, 4800);
        const double moved = energy(edited, 28800);
        return {{"readOk", read_ok},
                {"error", QString::fromStdString(error)},
                {"original", original},
                {"quieter", quieter},
                {"muted", muted},
                {"deleted", deleted},
                {"beforeMovedAudio", before_audio},
                {"moved", moved},
                {"usesVideoComposition", exosnap::quick::EditPlayerWorkerTestPeer::composed(worker)}};
    }
    Q_INVOKABLE QVariantMap measurementBridgeProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter replacement;
        EditPlayerAdapter player;
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        struct Gate {
            std::atomic<bool> entered{false};
            std::atomic<bool> release{false};
        };
        const auto gateWorker = [&] {
            auto gate = std::make_shared<Gate>();
            QMetaObject::invokeMethod(
                worker,
                [gate] {
                    QElapsedTimer limit;
                    limit.start();
                    gate->entered.store(true);
                    while (!gate->release.load() && limit.elapsed() < 2000)
                        QThread::msleep(1);
                },
                Qt::QueuedConnection);
            return gate;
        };
        const auto waitFor = [](const std::function<bool()>& done) {
            QElapsedTimer limit;
            limit.start();
            while (!done() && limit.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            return done();
        };
        auto gate = gateWorker();
        const bool gated = waitFor([&] { return gate->entered.load(); });
        QElapsedTimer cost;
        cost.start();
        const auto initial = player.measurementSnapshot();
        bool coalesced = true;
        for (int read = 0; read < 100; ++read) {
            const auto state = player.measurementSnapshot();
            coalesced &= state.value("requestId") == initial.value("requestId") && state.value("pending").toBool();
        }
        const bool nonblocking = cost.elapsed() < 50 && !gate->release.load();
        const bool pending = initial.value("status") == "pending" && !initial.value("sampleId").isValid();
        gate->release.store(true);
        QVariantMap captured;
        const bool ready = waitFor([&] {
            captured = player.measurementSnapshot();
            return captured.value("status") == "available";
        });
        const auto sample = captured.value("sampleId");
        const bool stamp = ready && sample.toULongLong() > 0 && captured.value("ageMs").toLongLong() >= 0 &&
                           !captured.value("capturedUtc").toString().isEmpty();
        const auto causes = captured.value("values").toMap().value("mediaHashByCause").toMap();
        bool by_cause = causes.size() == 7;
        for (const auto* name : {"unspecified", "source_probe", "audio_metadata", "sidecar", "recording_finalization",
                                 "live_lease", "decoder_validation"}) {
            const auto values = causes.value(QString::fromLatin1(name)).toMap();
            by_cause &= values.contains("starts") && values.contains("completions") && values.contains("bytes") &&
                        values.contains("cancellations") && values.contains("elapsedNs");
        }
        bool cadence = ready;
        for (int read = 0; read < 100; ++read)
            cadence &= player.measurementSnapshot().value("sampleId") == sample;
        gate = gateWorker();
        waitFor([&] { return gate->entered.load(); });
        QElapsedTimer age;
        age.start();
        while (age.elapsed() < 275)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        const auto stale = player.measurementSnapshot();
        const bool stale_label = stale.value("status") == "stale" && stale.value("sampleId") == sample &&
                                 stale.value("pending").toBool() && stale.value("ageMs").toLongLong() >= 250;
        player.setSession(&replacement);
        const auto replaced = player.measurementSnapshot();
        const bool invalidated = replaced.value("currentSessionId") != captured.value("currentSessionId") &&
                                 !replaced.value("sampleId").isValid();
        gate->release.store(true);
        QVariantMap refreshed;
        const bool renewed = waitFor([&] {
            refreshed = player.measurementSnapshot();
            return refreshed.value("status") == "available" &&
                   refreshed.value("sessionId") == replaced.value("currentSessionId");
        });
        return {{"gated", gated},         {"nonblocking", nonblocking}, {"pending", pending},
                {"coalesced", coalesced}, {"captured", stamp},          {"byCause", by_cause},
                {"cadence", cadence},     {"stale", stale_label},       {"oldSessionRejected", invalidated},
                {"refreshed", renewed}};
    }

    Q_INVOKABLE QVariantMap presentationStampProbe() {
        exosnap::quick::ExoEditPlayerItem item;
        exosnap::quick::EditPlayerFrameSink sink;
        sink.attach(&item);
        auto generation = sink.invalidate();
        sink.setRevision(generation, 11);
        exosnap::engine::RawDecodedVideoFrame raw;
        raw.pts_us = 100;
        sink.deliver(raw, generation);
        const auto first = item.takePendingFrame();
        const bool submitted = sink.outputRevision() == 11;
        const bool unavailable = !item.sceneGraphConsumedRevision().isValid();
        item.publishRenderStateFromRenderThread(first.generation, true, QSize(2, 2), {}, first.timeline_time_us,
                                                first.presentation);
        QCoreApplication::processEvents();
        const bool painted = item.sceneGraphConsumedRevision().toULongLong() == 11;
        generation = sink.invalidate();
        sink.setRevision(generation, 12);
        raw.pts_us = 200;
        sink.deliver(raw, generation);
        const auto obsolete = item.takePendingFrame();
        item.publishRenderStateFromRenderThread(obsolete.generation, true, QSize(2, 2), {}, obsolete.timeline_time_us,
                                                obsolete.presentation);
        generation = sink.invalidate();
        QCoreApplication::processEvents();
        const bool stale = item.sceneGraphConsumedRevision().toULongLong() == 11;
        sink.setRevision(generation, 13);
        sink.deliverTimeline({300, {2, 2}, {}}, generation);
        const auto current = item.takePendingFrame();
        item.publishRenderStateFromRenderThread(current.generation, true, QSize(2, 2), {}, current.timeline_time_us,
                                                current.presentation);
        sink.setRevision(generation, 14);
        QCoreApplication::processEvents();
        const bool payload = item.sceneGraphConsumedRevision().toULongLong() == 13;
        sink.invalidate();
        const bool retained = item.sceneGraphConsumedRevision().toULongLong() == 13;
        sink.clear();
        QCoreApplication::processEvents();
        const bool cleared = !item.sceneGraphConsumedRevision().isValid();
        return {{"submittedBeforePaint", submitted},      {"unavailableBeforePaint", unavailable},
                {"firstPaintStamped", painted},           {"stalePaintRejected", stale},
                {"payloadNotRequestedRevision", payload}, {"retainedOldPictureRevision", retained},
                {"clearMakesUnavailable", cleared}};
    }
    Q_INVOKABLE QVariantMap stagedRevisionProbe() {
        namespace edit = exosnap::edit;
        namespace engine = exosnap::engine;
        using exosnap::quick::EditPlayerWorkerTestPeer;
        EditSessionAdapter imported;
        imported.importMedia(QUrl::fromLocalFile(qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/red.mkv"),
                             true, 0);
        const auto wait_for = [](auto done) {
            QElapsedTimer limit;
            limit.start();
            while (!done() && limit.elapsed() < 5000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            return done();
        };
        if (!wait_for([&] { return imported.open() && !imported.workspace().clips().empty(); }))
            return {{"imported", false}, {"error", imported.workspaceError()}};
        auto authored = imported.workspace();
        const auto old_revision = authored.revision();
        std::vector<engine::TimelineMediaSource> sources;
        for (const auto& asset : authored.assets()) {
            const auto metadata = imported.sourceMetadata(asset.id);
            if (metadata)
                sources.push_back({asset.id, std::filesystem::path(asset.path), *metadata, asset.monitor_selection,
                                   imported.sourceAccess(asset.id), imported.sourceProof(asset.id)});
        }
        exosnap::quick::EditPlayerWorker worker(std::make_shared<exosnap::quick::EditPlayerFrameSink>());
        worker.setTimeline(std::make_shared<exosnap::quick::EditPlaybackSnapshot>(authored, sources), 0,
                           [] { return false; });
        const auto audible = EditPlayerWorkerTestPeer::prepareComponentAudio(worker);
        const bool ready = wait_for([&] { return audible->Measurements().source_buffer_frames >= 24000; });
        const auto old_generation = audible->Measurements().stream_generation;
        EditPlayerWorkerTestPeer::componentPlaying(worker);
        struct Gate {
            std::atomic<bool> entered{false};
            std::atomic<bool> release{false};
            std::atomic<bool> cancelled{false};
        };
        const auto gate = std::make_shared<Gate>();
        EditPlayerWorkerTestPeer::gateAudio(worker, [gate](const std::atomic<bool>& revoke) {
            gate->entered.store(true);
            while (!gate->release.load() && !revoke.load())
                QThread::msleep(1);
            gate->cancelled.store(revoke.load());
            return !revoke.load();
        });
        const auto video = std::find_if(authored.clips().begin(), authored.clips().end(), [&](const auto& clip) {
            const auto track = std::find_if(authored.tracks().begin(), authored.tracks().end(),
                                            [&](const auto& row) { return row.id == clip.track; });
            return track != authored.tracks().end() && track->type == edit::TrackType::Video;
        });
        if (video == authored.clips().end())
            return {{"imported", false}, {"error", "The fixture has no video clip."}};
        const bool applied = authored.apply(edit::SetClipOpacity{video->id, 0.5}, authored.revision()).status ==
                             edit::CommandStatus::Applied;
        worker.setTimeline(std::make_shared<exosnap::quick::EditPlaybackSnapshot>(authored, sources), 0,
                           [] { return true; });
        const bool preparing = wait_for([&] { return gate->entered.load(); });
        const bool retained_mapping =
            worker.performanceSnapshot().value("timelineRevision").toULongLong() == old_revision;
        std::array<float, 960> samples{};
        audible->Mix(old_generation, 0, samples);
        const bool retained_audio = std::any_of(samples.begin(), samples.end(), [](float value) { return value != 0; });
        float original_peak = 0;
        for (float sample : samples)
            original_peak = std::max(original_peak, std::abs(sample));
        const auto audio_clip = std::find_if(authored.clips().begin(), authored.clips().end(), [&](const auto& clip) {
            return std::any_of(authored.tracks().begin(), authored.tracks().end(), [&](const auto& track) {
                return track.id == clip.track && track.type == edit::TrackType::Audio;
            });
        });
        bool authored_gain = false;
        if (audio_clip != authored.clips().end() &&
            authored.apply(edit::SetGain{edit::GainTarget::Clip, audio_clip->id, -6.0}, authored.revision()).status ==
                edit::CommandStatus::Applied) {
            worker.setTimeline(std::make_shared<exosnap::quick::EditPlaybackSnapshot>(authored, sources), 0,
                               [] { return true; });
            audible->Mix(old_generation, 480, samples);
            float reduced_peak = 0;
            for (float sample : samples)
                reduced_peak = std::max(reduced_peak, std::abs(sample));
            authored_gain =
                original_peak > 0 && reduced_peak > original_peak * 0.45f && reduced_peak < original_peak * 0.55f;
        }
        const bool audio_mapping_revision =
            worker.performanceSnapshot().value("audioAppliedRevision").toULongLong() == old_revision;
        worker.setVolume(0);
        audible->Mix(old_generation, 960, samples);
        const bool muted = std::all_of(samples.begin(), samples.end(), [](float value) { return value == 0; });
        worker.pause();
        const bool pause_cancelled = wait_for([&] { return gate->cancelled.load(); });
        gate->release.store(true);
        EditPlayerWorkerTestPeer::gateAudio(worker, {});
        return {{"imported", true},
                {"originalReady", ready},
                {"editApplied", applied},
                {"replacementGated", preparing},
                {"oldMappingRetained", retained_mapping},
                {"oldAudioContinues", retained_audio},
                {"authoredGainWhilePending", authored_gain},
                {"oldAudioMappingRevisionRetained", audio_mapping_revision},
                {"muteWhilePending", muted},
                {"pauseCancelledReplacement", pause_cancelled}};
    }
    Q_INVOKABLE QVariantMap stagedRevisionCommitProbe() {
        namespace edit = exosnap::edit;
        namespace engine = exosnap::engine;
        using exosnap::quick::EditPlayerWorkerTestPeer;
        EditSessionAdapter imported;
        imported.importMedia(QUrl::fromLocalFile(qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/red.mkv"),
                             true, 0);
        const auto wait_for = [](auto done) {
            QElapsedTimer limit;
            limit.start();
            while (!done() && limit.elapsed() < 5000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            return done();
        };
        if (!wait_for([&] { return imported.open() && !imported.workspace().clips().empty(); }))
            return {{"imported", false}, {"error", imported.workspaceError()}};
        auto authored = imported.workspace();
        std::vector<engine::TimelineMediaSource> sources;
        for (const auto& asset : authored.assets())
            if (const auto metadata = imported.sourceMetadata(asset.id))
                sources.push_back({asset.id, std::filesystem::path(asset.path), *metadata, asset.monitor_selection,
                                   imported.sourceAccess(asset.id), imported.sourceProof(asset.id)});
        exosnap::quick::EditPlayerWorker worker(std::make_shared<exosnap::quick::EditPlayerFrameSink>());
        worker.setTimeline(std::make_shared<exosnap::quick::EditPlaybackSnapshot>(authored, sources), 0,
                           [] { return false; });
        const auto audible = EditPlayerWorkerTestPeer::prepareComponentAudio(worker);
        const bool ready = wait_for([&] { return audible->Measurements().source_buffer_frames >= 24000; });
        EditPlayerWorkerTestPeer::componentPlaying(worker);
        const auto video = std::find_if(authored.clips().begin(), authored.clips().end(), [&](const auto& clip) {
            const auto track = std::find_if(authored.tracks().begin(), authored.tracks().end(),
                                            [&](const auto& row) { return row.id == clip.track; });
            return track != authored.tracks().end() && track->type == edit::TrackType::Video;
        });
        if (video == authored.clips().end())
            return {{"imported", false}, {"error", "The fixture has no video clip."}};
        const auto video_id = video->id;
        EditPlayerWorkerTestPeer::unavailableEndpoint(worker);
        struct Gate {
            std::atomic<unsigned> entered{0};
            std::atomic<unsigned> cancelled{0};
            std::atomic<bool> release{false};
        };
        const auto gate = std::make_shared<Gate>();
        const auto admission = [](std::shared_ptr<Gate> gate) {
            return [gate](const std::atomic<bool>& revoke) {
                ++gate->entered;
                while (!gate->release.load() && !revoke.load())
                    QThread::msleep(1);
                if (revoke.load())
                    ++gate->cancelled;
                return !revoke.load();
            };
        };
        EditPlayerWorkerTestPeer::gateAudio(worker, admission(gate));
        bool applied = authored.apply(edit::SetClipOpacity{video_id, 0.5}, authored.revision()).status ==
                       edit::CommandStatus::Applied;
        worker.setTimeline(std::make_shared<exosnap::quick::EditPlaybackSnapshot>(authored, sources), 0,
                           [] { return true; });
        const bool first_gated = wait_for([&] { return gate->entered.load() != 0; });
        applied = authored.apply(edit::SetClipOpacity{video_id, 0.25}, authored.revision()).status ==
                      edit::CommandStatus::Applied &&
                  applied;
        worker.setTimeline(std::make_shared<exosnap::quick::EditPlaybackSnapshot>(authored, sources), 0,
                           [] { return true; });
        const bool superseded = wait_for([&] { return gate->cancelled.load() != 0 && gate->entered.load() >= 2; });
        QElapsedTimer held;
        held.start();
        while (held.elapsed() < 750)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        gate->release.store(true);
        const bool adopted = wait_for([&] {
            EditPlayerWorkerTestPeer::adoptPrepared(worker);
            return worker.performanceSnapshot().value("timelineRevision").toULongLong() == authored.revision();
        });
        const auto accepted = worker.performanceSnapshot();
        const bool current_position = EditPlayerWorkerTestPeer::position(worker) >= 700000 &&
                                      EditPlayerWorkerTestPeer::position(worker) < 1700000;
        const bool audible_latest = EditPlayerWorkerTestPeer::mixCurrent(worker);
        const auto next_gate = std::make_shared<Gate>();
        EditPlayerWorkerTestPeer::gateAudio(worker, admission(next_gate));
        applied = authored.apply(edit::SetClipOpacity{video_id, 0.75}, authored.revision()).status ==
                      edit::CommandStatus::Applied &&
                  applied;
        const auto still_playing = std::make_shared<std::atomic<bool>>(true);
        worker.setTimeline(std::make_shared<exosnap::quick::EditPlaybackSnapshot>(authored, sources), 0,
                           [still_playing] { return still_playing->load(); });
        const bool seek_gated = wait_for([&] { return next_gate->entered.load() != 0; });
        still_playing->store(false);
        EditPlayerWorkerTestPeer::adoptPrepared(worker);
        worker.requestTransport(31, 41, 1200000, true, false);
        const bool seek_cancelled = wait_for([&] { return next_gate->cancelled.load() != 0; });
        next_gate->release.store(true);
        const auto sought = worker.performanceSnapshot();
        EditPlayerWorkerTestPeer::gateAudio(worker, {});
        return {{"imported", true},
                {"originalReady", ready},
                {"editsApplied", applied},
                {"firstReplacementGated", first_gated},
                {"supersededReplacementCancelled", superseded},
                {"newestRevisionAdopted", adopted},
                {"adoptedAtCurrentClock", current_position},
                {"newestAudioApplied", audible_latest},
                {"unavailableOutputPreserved", accepted.value("audioOutputUnavailableReason").toString() ==
                                                   QStringLiteral("Injected unavailable component endpoint.")},
                {"boundedProducers", accepted.value("audioProducerInstances").toInt() <= 2},
                {"oneMappingHandoff", accepted.value("revisionHandoffs").toULongLong() == 1},
                {"seekReplacementGated", seek_gated},
                {"latestSeekCancelledPreparation", seek_cancelled},
                {"latestSeekRetainsAuthoredRevision",
                 sought.value("timelineRevision").toULongLong() == authored.revision()},
                {"latestSeekPosition", EditPlayerWorkerTestPeer::position(worker) == 1200000},
                {"latestSeekPaused",
                 EditPlayerWorkerTestPeer::paused(worker) && !sought.value("revisionPreparationPending").toBool()}};
    }
    Q_INVOKABLE QVariantMap videoOpenStallProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        using exosnap::quick::EditPlayerWorkerTestPeer;
        struct State {
            std::atomic<bool> prepared{false}, entered{false}, release{false}, cancelled{false};
            std::atomic<bool> command{false}, muted{false}, paused{false};
            std::shared_ptr<exosnap::engine::TimelineAudioProducer> producer;
            int64_t origin = 0;
        };
        const auto state = std::make_shared<State>();
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setVolume(0);
        player.setSession(&session);
        session.importMedia(QUrl::fromLocalFile(qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/red.mkv"),
                            true, 0);
        const auto wait_for = [](auto done, int timeout = 3000) {
            QElapsedTimer deadline;
            deadline.start();
            while (!done() && deadline.elapsed() < timeout)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            return done();
        };
        if (!wait_for([&] { return session.open() && player.clipOpen(); }, 5000))
            return {{"imported", false}, {"error", session.workspaceError()}};
        EditSessionAdapter replacement;
        replacement.importMedia(QUrl::fromLocalFile(qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/blue.mkv"),
                                true, 0);
        if (!wait_for([&] { return replacement.open() && !replacement.importPending(); }, 5000))
            return {{"imported", false}, {"error", replacement.workspaceError()}};
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        QMetaObject::invokeMethod(
            worker,
            [worker, state] {
                state->producer = EditPlayerWorkerTestPeer::prepareComponentAudio(*worker);
                EditPlayerWorkerTestPeer::componentPlaying(*worker);
                EditPlayerWorkerTestPeer::gateOpen(*worker, [state](const std::atomic<bool>& cancelled) {
                    state->entered.store(true);
                    while (!state->release.load() && !cancelled.load())
                        QThread::msleep(1);
                    state->cancelled.store(cancelled.load());
                    return !cancelled.load();
                });
                state->prepared.store(true, std::memory_order_release);
            },
            Qt::QueuedConnection);
        const bool original_ready = wait_for([&] {
            return state->prepared.load(std::memory_order_acquire) &&
                   state->producer->Measurements().source_buffer_frames >= 24000;
        });
        auto authored = replacement.workspace();
        std::vector<exosnap::engine::TimelineMediaSource> sources;
        for (const auto& asset : authored.assets())
            if (const auto metadata = replacement.sourceMetadata(asset.id))
                sources.push_back({asset.id, std::filesystem::path(asset.path), *metadata, asset.monitor_selection,
                                   replacement.sourceAccess(asset.id), replacement.sourceProof(asset.id)});
        const auto video = std::find_if(authored.clips().begin(), authored.clips().end(), [&](const auto& clip) {
            return std::any_of(authored.tracks().begin(), authored.tracks().end(), [&](const auto& track) {
                return track.id == clip.track && track.type == exosnap::edit::TrackType::Video;
            });
        });
        if (video == authored.clips().end()) {
            state->release.store(true);
            return {{"imported", false}, {"error", "The fixture has no video clip."}};
        }
        const bool edited = authored.apply(exosnap::edit::SetClipOpacity{video->id, 0.5}, authored.revision()).status ==
                            exosnap::edit::CommandStatus::Applied;
        const auto snapshot = std::make_shared<exosnap::quick::EditPlaybackSnapshot>(authored, sources);
        QMetaObject::invokeMethod(
            worker, [worker, snapshot] { worker->setTimeline(snapshot, 0, [] { return true; }); },
            Qt::QueuedConnection);
        const bool gated = wait_for([&] { return state->entered.load(); });
        QMetaObject::invokeMethod(
            worker,
            [worker, state] {
                worker->setVolume(0.5);
                state->producer = EditPlayerWorkerTestPeer::currentProducer(*worker);
                state->origin = EditPlayerWorkerTestPeer::currentAudioOrigin(*worker);
                state->command.store(true, std::memory_order_release);
            },
            Qt::QueuedConnection);
        const bool command = wait_for([&] { return state->command.load(std::memory_order_acquire); }, 500);
        bool audible = false, muted = false, paused = false;
        if (command && state->producer) {
            std::array<float, 960> samples{};
            const auto generation = state->producer->Measurements().stream_generation;
            state->producer->Mix(generation, state->origin, samples);
            audible = std::any_of(samples.begin(), samples.end(), [](float value) { return value != 0; });
            QMetaObject::invokeMethod(
                worker,
                [worker, state] {
                    worker->setVolume(0);
                    state->muted.store(true);
                },
                Qt::QueuedConnection);
            if (wait_for([&] { return state->muted.load(); }, 500)) {
                state->producer->Mix(generation, state->origin + 480, samples);
                muted = std::all_of(samples.begin(), samples.end(), [](float value) { return value == 0; });
            }
            QMetaObject::invokeMethod(
                worker,
                [worker, state] {
                    worker->pause();
                    state->paused.store(EditPlayerWorkerTestPeer::paused(*worker));
                },
                Qt::QueuedConnection);
            paused = wait_for([&] { return state->paused.load() && state->cancelled.load(); }, 500);
        }
        state->release.store(true);
        QMetaObject::invokeMethod(
            worker,
            [worker] {
                EditPlayerWorkerTestPeer::gateOpen(*worker, {});
                worker->pause();
            },
            Qt::BlockingQueuedConnection);
        return {{"imported", true},
                {"originalReady", original_ready},
                {"editApplied", edited},
                {"openGated", gated},
                {"commandWhileOpenStalled", command},
                {"audibleWhileOpenStalled", audible},
                {"mutedWhileOpenStalled", muted},
                {"pauseCancelledOpen", paused}};
    }
    Q_INVOKABLE QVariantMap videoStallProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        using exosnap::quick::EditPlayerWorkerTestPeer;
        struct State {
            std::atomic<bool> entered{false};
            std::atomic<bool> release{false};
            std::atomic<bool> cancelled{false};
            std::atomic<bool> command{false};
            std::atomic<bool> muted{false};
            std::shared_ptr<exosnap::engine::TimelineAudioProducer> producer;
        };
        auto state = std::make_shared<State>();
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setVolume(0);
        player.setSession(&session);
        session.importMedia(QUrl::fromLocalFile(qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/red.mkv"),
                            true, 0);
        QElapsedTimer deadline;
        const auto waitFor = [&](const std::function<bool()>& done, int timeout = 3000) {
            deadline.restart();
            while (!done() && deadline.elapsed() < timeout)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            return done();
        };
        if (!waitFor([&] { return session.open() && player.clipOpen(); }, 5000))
            return {{"imported", false}, {"error", session.workspaceError()}};
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        QMetaObject::invokeMethod(
            worker,
            [worker, state] {
                EditPlayerWorkerTestPeer::gateVideo(*worker, [state](const std::atomic<bool>& cancelled) {
                    if (state->entered.exchange(true))
                        return !cancelled.load();
                    while (!state->release.load() && !cancelled.load())
                        QThread::msleep(1);
                    state->cancelled.store(cancelled.load());
                    return !cancelled.load();
                });
            },
            Qt::BlockingQueuedConnection);
        session.setClipFade(session.workspace().clips().front().id, true, 200);
        const bool gated = waitFor([&] { return state->entered.load(); });
        QMetaObject::invokeMethod(
            worker,
            [worker, state] {
                worker->setVolume(0.5);
                state->producer = EditPlayerWorkerTestPeer::prepareComponentAudio(*worker);
                state->command.store(true, std::memory_order_release);
            },
            Qt::QueuedConnection);
        const bool command = waitFor([&] { return state->command.load(std::memory_order_acquire); }, 500);
        bool ready = false;
        bool audible = false;
        bool muted = false;
        bool applied = false;
        if (command) {
            ready = waitFor([&] { return state->producer->Measurements().source_buffer_frames >= 24000; });
            std::array<float, 960> samples{};
            const auto generation = state->producer->Measurements().stream_generation;
            state->producer->Mix(generation, 0, samples);
            audible = std::any_of(samples.begin(), samples.end(), [](float sample) { return sample != 0; });
            QMetaObject::invokeMethod(
                worker,
                [worker, state] {
                    worker->setVolume(0);
                    state->muted.store(true);
                },
                Qt::QueuedConnection);
            if (waitFor([&] { return state->muted.load(); }, 500)) {
                state->producer->Mix(generation, 480, samples);
                muted = std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0; });
                const auto measurements = state->producer->Measurements();
                applied = measurements.applied_control_revision == measurements.requested_control_revision;
            }
        }
        session.requestSeekUs(500'000);
        const bool cancelled = command && waitFor([&] { return state->cancelled.load(); }, 500);
        state->release.store(true);
        QMetaObject::invokeMethod(
            worker, [worker] { EditPlayerWorkerTestPeer::gateVideo(*worker, {}); }, Qt::BlockingQueuedConnection);
        return {{"imported", true},
                {"gated", gated},
                {"commandWhileVideoStalled", command},
                {"sourceReady", ready},
                {"audibleBeforeMute", audible},
                {"mutedWhileVideoStalled", muted},
                {"controlApplied", applied},
                {"seekCancelledVideo", cancelled}};
    }

    Q_INVOKABLE QVariantMap retainedPausedPictureProbe() {
        using exosnap::quick::EditPlayerAdapterTestPeer;
        EditSessionAdapter session;
        EditPlayerAdapter player;
        exosnap::quick::ExoEditPlayerItem item;
        player.setVolume(0);
        player.setSession(&session);
        item.setPlayerAdapter(&player);
        session.importMedia(QUrl::fromLocalFile(qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + "/red.mkv"),
                            true, 0);
        const auto wait_for = [](auto done) {
            QElapsedTimer deadline;
            deadline.start();
            while (!done() && deadline.elapsed() < 5000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            return done();
        };
        if (!wait_for([&] {
                return session.open() && !session.workspace().assets().empty() &&
                       session.sourceProof(session.workspace().assets().front().id);
            })) {
            item.setPlayerAdapter(nullptr);
            return {{"initialReady", false}, {"error", session.workspaceError()}};
        }
        player.setPlaying(false);
        session.requestSeekUs(250000);
        const bool ready = wait_for([&] {
            auto pending = item.takePendingFrame();
            if (pending.frame && pending.timeline_time_us && std::abs(*pending.timeline_time_us - 250000) < 50000) {
                item.publishRenderStateFromRenderThread(
                    pending.generation, true,
                    QSize(static_cast<int>(pending.frame->width), static_cast<int>(pending.frame->height)), {},
                    pending.timeline_time_us);
            }
            return player.previewState() == EditPlayerAdapter::Ready && !player.seekPending();
        });
        const auto audio =
            std::find_if(session.workspace().clips().begin(), session.workspace().clips().end(), [&](const auto& clip) {
                return clip.stream && clip.stream->kind == exosnap::engine::TimelineStreamKind::Audio;
            });
        if (audio == session.workspace().clips().end()) {
            item.setPlayerAdapter(nullptr);
            return {{"initialReady", ready}, {"editApplied", false}};
        }
        const auto generation = EditPlayerAdapterTestPeer::generation(player);
        const auto shown = player.presentedTimeUs();
        const auto audio_id = audio->id;
        const bool edited = session.setClipGain(audio_id, -6);
        auto* worker = EditPlayerAdapterTestPeer::worker(player);
        QMetaObject::invokeMethod(worker, [] {}, Qt::BlockingQueuedConnection);
        QCoreApplication::processEvents();
        QVariantMap result{{"initialReady", ready},
                           {"editApplied", edited},
                           {"pausedReadyRetained", player.previewState() == EditPlayerAdapter::Ready},
                           {"generationRetained", EditPlayerAdapterTestPeer::generation(player) == generation},
                           {"shownTimeRetained", player.presentedTimeUs() == shown},
                           {"seekPending", player.seekPending()},
                           {"paused", !player.desiredPlaying()}};
        const auto error_command = EditPlayerAdapterTestPeer::command(player);
        QMetaObject::invokeMethod(
            worker,
            [worker, error_command, generation] {
                emit worker->openFinishedForCommand(error_command, generation, true,
                                                    QStringLiteral("Injected retained-picture render failure."));
            },
            Qt::QueuedConnection);
        const bool degraded = wait_for([&] { return player.previewState() == EditPlayerAdapter::Degraded; });
        const auto reason = player.placeholderText();
        result["initialDegraded"] = degraded && !reason.isEmpty();
        result["secondEditApplied"] = session.setClipGain(audio_id, -3);
        QMetaObject::invokeMethod(worker, [] {}, Qt::BlockingQueuedConnection);
        QCoreApplication::processEvents();
        result["degradedStateRetained"] = player.previewState() == EditPlayerAdapter::Degraded;
        result["degradedReasonRetained"] = player.placeholderText() == reason;
        item.setPlayerAdapter(nullptr);
        return result;
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
        QStringList monitoring_errors;
        connect(worker, &exosnap::quick::EditPlayerWorker::monitoringRejected, &player, [&](const QString& error) {
            ++monitoring_rejections;
            monitoring_errors.push_back(error);
        });
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
        deadline.restart();
        while (player.monitoringTracks().empty() && deadline.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
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
                {"monitoringErrors", monitoring_errors},
                {"opens",
                 (after.value("sessionOpens").toULongLong() + after.value("videoDecoderOpens").toULongLong()) -
                     (before.value("sessionOpens").toULongLong() + before.value("videoDecoderOpens").toULongLong())}};
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
        session.nudgeSelected(17);
        const auto revision_a = session.workspace().revision();
        session.requestSeekUs(250000);
        QVariantMap at_seek;
        QMetaObject::invokeMethod(worker, [&]() { at_seek = worker->performanceSnapshot(); }, Qt::QueuedConnection);
        session.nudgeSelected(17);
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
                 (after.value("sessionOpens").toULongLong() + after.value("videoDecoderOpens").toULongLong()) -
                     (before.value("sessionOpens").toULongLong() + before.value("videoDecoderOpens").toULongLong())}};
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
        const auto verified_sources = [&] {
            return std::all_of(session.workspace().assets().begin(), session.workspace().assets().end(),
                               [&](const auto& asset) { return bool(session.sourceProof(asset.id)); });
        };
        while (!verified_sources() && deadline.elapsed() < 5'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!verified_sources())
            return {{"imported", true}, {"failure", QStringLiteral("Source verification did not complete")}};
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

class PreviewWorkload final : public QObject {
    Q_OBJECT
    Q_PROPERTY(exosnap::quick::EditSessionAdapter* session READ session CONSTANT)
    Q_PROPERTY(exosnap::quick::EditTimelineAdapter* timeline READ timeline CONSTANT)

  public:
    explicit PreviewWorkload(QObject* parent) : QObject(parent) {
        exosnap::ThumbnailRequestBroker::Config config;
        config.disk_enabled = false;
        timeline_.configureThumbnailCache(config);
        timeline_.useSyntheticMediaForHarness(
            [](const exosnap::ThumbnailSourceInfo& source, std::int64_t, int height) {
                QImage image(std::max(1, height * 16 / 9), height, QImage::Format_ARGB32_Premultiplied);
                image.fill(QColor(source.content_sha256[0], 80, 120));
                return image;
            },
            {}, std::filesystem::path(cache_.path().toStdWString()));
    }
    exosnap::quick::EditSessionAdapter* session() {
        return &session_;
    }
    exosnap::quick::EditTimelineAdapter* timeline() {
        return &timeline_;
    }
    Q_INVOKABLE qulonglong thumbnailQueries() const {
        return timeline_.thumbnailQueriesForTest();
    }
    Q_INVOKABLE qulonglong modelQueries(QObject* object) const {
        const auto* model = qobject_cast<exosnap::quick::EditTimelineClipModel*>(object);
        return model ? model->syncCountForTest() : 0;
    }
    Q_INVOKABLE bool hasWaveformRequest(const QString& key) const {
        const auto* service = qobject_cast<exosnap::WaveformCacheService*>(timeline_.waveformService());
        return service && service->state(key) != exosnap::WaveformCacheService::State::Unknown;
    }
    Q_INVOKABLE bool prepare(int tracks) {
        return prepareSources(tracks, false);
    }
    Q_INVOKABLE bool prepareAudio() {
        return prepareSources(1, true);
    }
    bool prepareSources(int tracks, bool audio) {
        if (!cache_.isValid() || tracks < 1 || tracks > 2000)
            return false;
        session_.close();
        timeline_.clearMediaCaches();
        session_.snapping = false;
        session_.autoCrossfade = false;
        for (int index = 0; index < tracks; ++index) {
            exosnap::engine::EditMediaMetadata metadata;
            metadata.content_identity_valid = true;
            metadata.has_audio = audio;
            metadata.content_sha256.fill(42);
            metadata.content_sha256[0] = static_cast<std::uint8_t>(index & 255);
            metadata.content_sha256[1] = static_cast<std::uint8_t>((index >> 8) & 255);
            metadata.duration_us = 60'000'000;
            metadata.width = audio ? 0 : 1280;
            metadata.height = audio ? 0 : 720;
            metadata.fps_num = 30;
            metadata.fps_den = 1;
            exosnap::engine::EditSourceStream stream;
            stream.kind =
                audio ? exosnap::engine::TimelineStreamKind::Audio : exosnap::engine::TimelineStreamKind::Video;
            stream.codec.channels = audio ? 2 : 0;
            stream.codec.sample_rate = audio ? 48'000 : 0;
            stream.fps_num = 30;
            stream.fps_den = 1;
            metadata.streams.push_back(stream);
            const auto asset = session_.addProbedMediaForHarness(
                cache_.filePath(QStringLiteral("source-%1.mkv").arg(index)), metadata, true);
            if (!audio &&
                !timeline_.registerThumbnailSource(session_.sourcePreviewIdentity(asset),
                                                   cache_.filePath(QStringLiteral("source-%1.mkv").arg(index)),
                                                   metadata.width, metadata.height, metadata.fps_num, metadata.fps_den))
                return false;
            if (session_.workspace().clips().empty())
                return false;
            if (index > 0) {
                const auto& clips = session_.workspace().clips();
                const auto found = std::find_if(clips.begin(), clips.end(),
                                                [asset](const auto& value) { return value.asset == asset; });
                if (found == clips.end())
                    return false;
                const auto clip = *found;
                session_.beginClipDrag(clip.id);
                session_.updateClipDragUs(-clip.start, 0, 1, false, 0);
                if (!session_.commitClipDrag())
                    return false;
            }
            if (!session_.workspaceError().isEmpty())
                return false;
        }
        const auto& clips = session_.workspace().clips();
        if (clips.size() != static_cast<std::size_t>(tracks))
            return false;
        std::set<exosnap::edit::Id> occupied;
        for (const auto& clip : clips) {
            if (clip.start != 0)
                return false;
            occupied.insert(clip.track);
        }
        return occupied.size() == static_cast<std::size_t>(tracks);
    }

  private:
    QTemporaryDir cache_;
    exosnap::quick::EditSessionAdapter session_;
    exosnap::quick::EditTimelineAdapter timeline_;
};
class Setup final : public QObject {
    Q_OBJECT

  public slots:
    bool prepareRecordingReviewVisual() {
        const auto path = exosnap::quick::test::TransportAmbiguousFixture();
        exosnap::engine::EditPlayerEngine decoder;
        std::string error;
        if (!review_ || path.isEmpty() ||
            !decoder.OpenVideoStream(std::filesystem::path(path.toStdWString()), 0, error))
            return false;
        const auto frame = decoder.DecodeFrameAt(700'000);
        if (!frame || !frame->bgra)
            return false;
        QImage image(frame->bgra.get(), static_cast<int>(frame->width), static_cast<int>(frame->height),
                     static_cast<int>(frame->stride_bytes), QImage::Format_ARGB32);
        if (!review_visual_directory_.isValid())
            return false;
        const auto still_path = review_visual_directory_.filePath(QStringLiteral("frame.png"));
        if (!image.save(still_path, "PNG"))
            return false;
        auto context = FixtureContext();
        context.duration_seconds = 2.0;
        review_->open(context);
        review_->player()->showStillForHarness(QUrl::fromLocalFile(still_path), 700'000, 2000);
        // Close notifications must finish before the static monitoring rows are published.
        QMetaObject::invokeMethod(
            exosnap::quick::EditPlayerAdapterTestPeer::worker(*review_->player()), [] {}, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(review_->player(), QEvent::MetaCall);
        review_->player()->setClipStateForTest(true, 2000);
        review_->session()->requestSeek(700);
        review_->setVolume(0.72);
        review_->setMuted(false);
        review_->player()->setMonitoringState({QVariantMap{{"id", "first"},
                                                           {"name", "Audio 1"},
                                                           {"sources", "Sources unknown"},
                                                           {"selected", true},
                                                           {"available", true}},
                                               QVariantMap{{"id", "second"},
                                                           {"name", "Audio 2"},
                                                           {"sources", "Sources unknown"},
                                                           {"selected", false},
                                                           {"available", true}}},
                                              false);
        return true;
    }

    bool prepareRecordingReview() {
        const auto path = exosnap::quick::test::TransportAmbiguousFixture();
        if (!review_ || path.isEmpty())
            return false;
        auto context = FixtureContext();
        context.mkv_master_path = path;
        context.output_path = path;
        context.duration_seconds = 2.0;
        review_->setMuted(true);
        review_->open(context);
        review_->player()->setSurfaceVisible(true);
        return true;
    }

    void setReviewPlaybackPlaying(bool playing) {
        exosnap::quick::EditPlayerAdapterTestPeer::acknowledge(*transport_player_, playing);
    }

    QString prepareReviewHandoff() {
        if (!page_recordings_ || !review_)
            return {};
        review_->close();
        {
            QFile source(exosnap::quick::test::TransportAmbiguousFixture());
            QTemporaryFile output(review_visual_directory_.filePath(QStringLiteral("recording-XXXXXX.mkv")));
            if (!source.open(QIODevice::ReadOnly) || !output.open())
                return {};
            const auto bytes = source.readAll();
            if (output.write(bytes) != bytes.size() || !output.flush())
                return {};
            output.setAutoRemove(false);
            reviewed_recording_path_ = output.fileName();
            output.close();
        }
        record_view_model_.recent_recordings.clear();
        for (int i = 0; i < 24; ++i) {
            exosnap::CompletedRecording other;
            other.file_path = review_visual_directory_.filePath(QStringLiteral("Other %1.mkv").arg(i));
            record_view_model_.recent_recordings.push_back(other);
        }
        exosnap::CompletedRecording recording;
        recording.file_path = reviewed_recording_path_;
        recording.duration_seconds = 2.0;
        recording.succeeded = true;
        record_view_model_.recent_recordings.push_back(recording);
        page_recordings_->synchronize();
        auto context = FixtureContext();
        context.output_path = reviewed_recording_path_;
        context.mkv_master_path = reviewed_recording_path_;
        context.duration_seconds = 2.0;
        review_->setMuted(true);
        review_->open(context);
        return exosnap::quick::RecordViewModelAdapter::recordingIdentity(reviewed_recording_path_);
    }

    void removeReviewedHistory() {
        record_view_model_.recent_recordings.clear();
        page_recordings_->synchronize();
    }

    QVariantMap recordingIdentityProbe() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("recording.mkv"));
        QFile original(path);
        if (!original.open(QIODevice::WriteOnly) || original.write("first") != 5)
            return {};
        original.close();
        const auto first = exosnap::quick::RecordViewModelAdapter::recordingIdentity(path);
        if (!original.rename(directory.filePath(QStringLiteral("renamed.mkv"))))
            return {};
        const auto renamed = exosnap::quick::RecordViewModelAdapter::recordingIdentity(original.fileName());
        QFile replacement(path);
        if (!replacement.open(QIODevice::WriteOnly) || replacement.write("other") != 5)
            return {};
        replacement.close();
        const auto second = exosnap::quick::RecordViewModelAdapter::recordingIdentity(path);
        if (!replacement.remove())
            return {};
        return {{"stable", !first.isEmpty() && first == renamed},
                {"replaced", !second.isEmpty() && first != second},
                {"missing", exosnap::quick::RecordViewModelAdapter::recordingIdentity(path).isEmpty()}};
    }

    void seedReviewEditorWorkspace() {
        page_session_->close();
        page_session_->setEditContext(FixtureContext());
        page_session_->setEditContext(FixtureContext());
    }

    QVariantMap reviewEditorCheckpoint() const {
        return {{"revision", QVariant::fromValue<qulonglong>(page_session_->workspace().revision())},
                {"historyBytes", QVariant::fromValue<qulonglong>(page_session_->workspace().historyBytes())},
                {"duration", page_session_->durationUs()},
                {"position", page_session_->positionUs()},
                {"selected", page_session_->selectedClipIds()},
                {"selectionMode", page_session_->selectionMode()},
                {"undo", page_session_->canUndo()},
                {"redo", page_session_->canRedo()},
                {"media", page_session_->media()}};
    }

    QVariantMap reviewLifecycleProbe() {
        exosnap::quick::RecordingReviewAdapter review;
        auto context = FixtureContext();
        const auto initial = session_->workspace().revision();
        review.open(context);
        const bool opened = review.active();
        review.setVolume(0.72);
        review.setMuted(true);
        const bool silent = review.player()->volume() == 0.0 && review.volume() == 0.72;
        review.setVolume(0.35);
        const bool held = review.player()->volume() == 0.0;
        review.setMuted(false);
        const bool restored = review.player()->volume() == 0.35;
        review.setVolume(2.0);
        const bool clamped = review.volume() == 1.0;
        review.player()->beginScrub();
        review.close();
        const bool scrubEnded = !review.player()->scrubbing();
        EditSessionAdapter editor;
        editor.setEditContextUs(context, -1, 700'000);
        const bool positioned = editor.positionMs() == 700;
        editor.setEditContextUs(context, -1, 300'000);
        const bool appended = editor.positionMs() == 100'300;
        EditSessionAdapter async_editor;
        auto media_context = context;
        media_context.mkv_master_path = exosnap::quick::test::TransportAmbiguousFixture();
        media_context.output_path = media_context.mkv_master_path;
        media_context.duration_seconds = 2.0;
        async_editor.setEditContextUs(media_context, -1, 700'000);
        async_editor.close();
        async_editor.setEditContextUs(media_context, -1, 900'000);
        QElapsedTimer deadline;
        deadline.start();
        while (async_editor.importPending() && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        const bool async_positioned = !async_editor.importPending() && async_editor.positionMs() == 900;
        const bool cancelled_import =
            async_editor.durationMs() == 2000 && async_editor.workspace().assets().size() == 1;
        EditPlayerAdapter failed_player;
        exosnap::quick::ExoEditPlayerItem recovered_surface;
        recovered_surface.setPlayerAdapter(&failed_player);
        failed_player.applyPreviewStateForHarness(EditPlayerAdapter::Failed, QStringLiteral("Decode failed"));
        emit recovered_surface.framePresented(0);
        const bool recovered =
            failed_player.previewState() == EditPlayerAdapter::Ready && failed_player.placeholderText().isEmpty();
        return {{"opened", opened},
                {"silent", silent},
                {"held", held},
                {"restored", restored},
                {"clamped", clamped},
                {"scrubEnded", scrubEnded},
                {"positioned", positioned},
                {"appended", appended},
                {"recovered", recovered},
                {"asyncPositioned", async_positioned},
                {"cancelledImport", cancelled_import},
                {"closed", !review.active() && !review.player()->surfaceVisible()},
                {"residentUnchanged", session_->workspace().revision() == initial}};
    }

    QVariantMap prepareSourceAudioChoice() {
        const auto fixture = exosnap::quick::test::TransportAmbiguousFixture();
        if (!page_session_ || fixture.isEmpty())
            return {{"ready", false},
                    {"reason", QStringLiteral("The ambiguous-transport fixture could not be generated.")}};
        page_session_->close();
        page_session_->importMedia(QUrl::fromLocalFile(fixture), true);
        QElapsedTimer deadline;
        deadline.start();
        while (page_session_->importPending() && deadline.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        const auto rows = page_session_->media();
        if (rows.size() != 1)
            return {{"ready", false}, {"reason", page_session_->workspaceError()}};
        const auto asset = rows.front().toMap().value(QStringLiteral("id")).toULongLong();
        while (!page_session_->sourceProof(asset) && !page_session_->sourceVerificationFailed(asset) &&
               deadline.elapsed() < 10000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        const auto access = page_session_->sourceAccess(asset);
        const bool ready = access && page_session_->sourceProof(asset) && page_session_->beginSourceAudioChoice(asset);
        return {{"ready", ready},
                {"assetId", QVariant::fromValue<qulonglong>(asset)},
                {"revision", QVariant::fromValue<qulonglong>(page_session_->workspace().revision())},
                {"hashStarts", QVariant::fromValue<qulonglong>(access ? access->Measurements().hash_starts : 0)},
                {"reason", page_session_->sourceVerificationReason(asset)}};
    }
    void clearSourceAudioChoiceFixture() {
        if (page_session_)
            page_session_->close();
    }
    bool prepareViewerState(QObject* object, int state) const {
        auto* player = qobject_cast<EditPlayerAdapter*>(object);
        if (!player)
            return false;
        QImage still(4, 4, QImage::Format_RGB32);
        still.fill(Qt::black);
        QByteArray encoded;
        QBuffer buffer(&encoded);
        if (!buffer.open(QIODevice::WriteOnly) || !still.save(&buffer, "PNG"))
            return false;
        const QUrl source(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(encoded.toBase64()));
        player->showStillForHarness(source, 42'500'000, 160'000);
        player->applyPreviewStateForHarness(static_cast<EditPlayerAdapter::PreviewState>(state));
        return player->previewState() == state;
    }
    QVariantMap syntheticViewerStatesProbe() const {
        EditPlayerAdapter player;
        const QUrl image(QStringLiteral("file:///synthetic-visual-frame.png"));
        player.showStillForHarness(image, 42'500'000, 160'000);
        const auto command = exosnap::quick::EditPlayerAdapterTestPeer::command(player);
        const auto generation = exosnap::quick::EditPlayerAdapterTestPeer::generation(player);
        bool states = true;
        bool reasons = true;
        bool silent = true;
        for (const auto state : {EditPlayerAdapter::Opening, EditPlayerAdapter::Buffering, EditPlayerAdapter::Degraded,
                                 EditPlayerAdapter::Failed}) {
            player.showStillForHarness(image, 42'500'000, 160'000);
            const auto reason = QStringLiteral("Visual state %1").arg(state);
            player.applyPreviewStateForHarness(state, reason);
            states = states && player.previewState() == state;
            reasons = reasons && player.placeholderText() == reason;
            const bool retains_picture = state == EditPlayerAdapter::Degraded || state == EditPlayerAdapter::Failed;
            silent = silent &&
                     (retains_picture ? player.harnessStillSource() == image : player.harnessStillSource().isEmpty()) &&
                     !player.presentedTimeUs().isValid() && !player.seekPending() && !player.playing();
        }
        player.setLoopRangeForHarness(10'000'000, 20'000'000);
        const auto region = player.loopRegion();
        return {{"states", states},
                {"reasons", reasons},
                {"retainedUnmeasuredStill", silent},
                {"loop", player.loopEnabled() && region.value(QStringLiteral("startUs")).toLongLong() == 10'000'000 &&
                             region.value(QStringLiteral("endUs")).toLongLong() == 20'000'000},
                {"noTransport", command == exosnap::quick::EditPlayerAdapterTestPeer::command(player) &&
                                    generation == exosnap::quick::EditPlayerAdapterTestPeer::generation(player)}};
    }
    QVariantMap syntheticStillProbe() const {
        EditSessionAdapter session;
        EditPlayerAdapter player;
        player.setSession(&session);
        const QUrl image(QStringLiteral("file:///synthetic-visual-frame.png"));
        player.showStillForHarness(image, 42'500'000, 160'000);
        player.setMonitoringState({QVariantMap{{"id", "synthetic"}, {"available", true}}}, false);
        QMetaObject::invokeMethod(
            exosnap::quick::EditPlayerAdapterTestPeer::worker(player), [] {}, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&player, QEvent::MetaCall);
        return {{QStringLiteral("source"), player.harnessStillSource()},
                {QStringLiteral("requestedTimeUs"), player.requestedTimeUs()},
                {QStringLiteral("hasPresentedTime"), player.presentedTimeUs().isValid()},
                {QStringLiteral("seekPending"), player.seekPending()},
                {QStringLiteral("clipOpen"), player.clipOpen()},
                {QStringLiteral("playing"), player.playing()},
                {QStringLiteral("monitoringTrackCount"), player.monitoringTracks().size()}};
    }
    bool dropAsset(QQuickItem* item, qulonglong asset, qreal x, qreal y) {
        QMimeData mime;
        mime.setData(QStringLiteral("application/x-exosnap-asset"), QByteArray::number(asset));
        const auto position = item->mapToScene(QPointF(x, y));
        QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QDragMoveEvent move(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QDropEvent drop(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(item->window(), &enter);
        QCoreApplication::sendEvent(item->window(), &move);
        QCoreApplication::sendEvent(item->window(), &drop);
        return drop.isAccepted();
    }
    // A held library drag: enter (once) and move without dropping, so frame-driven
    // behaviour such as edge auto-scroll can run between the calls.
    void hoverAsset(QQuickItem* item, qulonglong asset, qreal x, qreal y) {
        const auto position = item->mapToScene(QPointF(x, y));
        if (!held_drag_) {
            held_mime_.setData(QStringLiteral("application/x-exosnap-asset"), QByteArray::number(asset));
            QDragEnterEvent enter(position, Qt::CopyAction, &held_mime_, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(item->window(), &enter);
            held_drag_ = true;
        }
        QDragMoveEvent move(position, Qt::CopyAction, &held_mime_, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(item->window(), &move);
    }
    void leaveHeldDrag(QQuickItem* item) {
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(item->window(), &leave);
        held_drag_ = false;
    }
    bool dropHeldDrag(QQuickItem* item, qreal x, qreal y) {
        QDropEvent drop(item->mapToScene(QPointF(x, y)), Qt::CopyAction, &held_mime_, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(item->window(), &drop);
        held_drag_ = false;
        return drop.isAccepted();
    }
    void repeatSpace(QQuickItem* item) {
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, {}, true);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, {}, true);
        QCoreApplication::sendEvent(item->window(), &release);
        QCoreApplication::sendEvent(item->window(), &press);
    }
    void deactivateWindow(QQuickItem* item) {
        QEvent event(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(item->window(), &event);
    }
    qulonglong transportCommand(QObject* object) const {
        const auto* player = qobject_cast<EditPlayerAdapter*>(object);
        return player ? exosnap::quick::EditPlayerAdapterTestPeer::command(*player) : 0;
    }
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
    // The request the control channel raises for a panel, through the shell.
    void requestEditPanel(int panel, bool open, bool pinned) {
        emit page_shell_->editPanelRequested(static_cast<exosnap::quick::ShellAdapter::EditPanel>(panel), open, pinned);
    }
    void requestEditSourceTab(int tab) {
        emit page_shell_->editSourceTabRequested(static_cast<exosnap::quick::ShellAdapter::EditSourceTab>(tab));
    }
    bool setVisualAppearance(const QString& appearance) {
        if (!visual_engine_)
            return false;
        auto* tokens = visual_engine_->singletonInstance<exosnap::quick::QuickThemeTokens*>(
            "ExoSnap.Quick.EditTestControls", "QuickThemeTokens");
        if (!tokens)
            return false;
        tokens->setAppearance(appearance, QStringLiteral("teal"));
        return tokens->appearanceId() == appearance;
    }
    void applicationAvailable() {
        if (qEnvironmentVariable("QSG_RHI_BACKEND") == QStringLiteral("d3d11"))
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
        QCoreApplication::setApplicationName(QStringLiteral("edit-timeline-qml-tests"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        if (qEnvironmentVariable("EXOSNAP_EDIT_VISUAL_LANGUAGE") == QStringLiteral("de")) {
            auto* translator = new QTranslator(QCoreApplication::instance());
            if (translator->load(qEnvironmentVariable("EXOSNAP_EDIT_VISUAL_CATALOG"))) {
                QCoreApplication::installTranslator(translator);
                QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
                QCoreApplication::instance()->setProperty("editVisualLanguage", QStringLiteral("de"));
            }
        }
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        visual_engine_ = engine;
        engine->rootContext()->setContextProperty(
            QStringLiteral("testVisualLanguage"),
            QCoreApplication::instance()->property("editVisualLanguage").toString());
        engine->rootContext()->setContextProperty(QStringLiteral("testPersistenceStage"),
                                                  qEnvironmentVariable("EXOSNAP_EDIT_PERSISTENCE_STAGE"));
        engine->rootContext()->setContextProperty(QStringLiteral("testVisualDirectory"),
                                                  qEnvironmentVariable("EXOSNAP_EDIT_VISUAL_DIR"));
        engine->rootContext()->setContextProperty(QStringLiteral("testScale"),
                                                  qEnvironmentVariable("QT_SCALE_FACTOR", "1"));
        engine->rootContext()->setContextProperty(QStringLiteral("testMediaPreview"), new MediaPreviewDriver(this));
        auto* previews = new PreviewWorkload(engine);
        engine->addImageProvider(QString::fromLatin1(exosnap::quick::kEditThumbnailProviderId),
                                 previews->timeline()->createThumbnailImageProvider());
        engine->rootContext()->setContextProperty(QStringLiteral("testPreviewWorkload"), previews);
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

        auto* loop_session = new EditSessionAdapter(this);
        auto* loop_player = new EditPlayerAdapter(this);
        loop_player->setSession(loop_session);
        const auto loop_fixture =
            qEnvironmentVariable("EXOSNAP_EDIT_RENDER_FIXTURES") + QStringLiteral("/qualification-60s-bt709.mkv");
        const bool real_loop_fixture = QFileInfo::exists(loop_fixture);
        if (real_loop_fixture)
            loop_session->importMedia(QUrl::fromLocalFile(loop_fixture), true);
        else
            loop_session->setEditContext(FixtureContext());
        engine->rootContext()->setContextProperty(QStringLiteral("testLoopRealMedia"), real_loop_fixture);
        engine->rootContext()->setContextProperty(QStringLiteral("testLoopSession"), loop_session);
        engine->rootContext()->setContextProperty(QStringLiteral("testLoopPlayer"), loop_player);

        auto* clipboard_session = new EditSessionAdapter(this);
        exosnap::engine::EditMediaMetadata clipboard_metadata;
        clipboard_metadata.duration_us = 10'000'000;
        clipboard_metadata.fps_num = 60;
        clipboard_metadata.fps_den = 1;
        clipboard_metadata.has_audio = true;
        clipboard_metadata.content_identity_valid = true;
        clipboard_metadata.content_sha256.fill(27);
        exosnap::engine::EditSourceStream clipboard_video;
        clipboard_video.kind = exosnap::engine::TimelineStreamKind::Video;
        clipboard_video.container_index = 0;
        clipboard_video.fps_num = 60;
        clipboard_video.fps_den = 1;
        exosnap::engine::EditSourceStream clipboard_audio;
        clipboard_audio.kind = exosnap::engine::TimelineStreamKind::Audio;
        clipboard_audio.container_index = 1;
        clipboard_audio.codec.channels = 2;
        clipboard_metadata.streams = {clipboard_video, clipboard_audio};
        clipboard_session->addProbedMediaForHarness(QStringLiteral("clipboard-source.mkv"), clipboard_metadata, true);
        engine->rootContext()->setContextProperty(QStringLiteral("testClipboardSession"), clipboard_session);

        auto* channel_session = new EditSessionAdapter(this);
        exosnap::engine::EditMediaMetadata channel_metadata;
        channel_metadata.content_identity_valid = true;
        channel_metadata.content_sha256.fill(37);
        channel_metadata.duration_us = 2'000'000;
        channel_metadata.has_audio = true;
        for (const auto channels : {1u, 2u}) {
            exosnap::engine::EditSourceStream stream;
            stream.kind = exosnap::engine::TimelineStreamKind::Audio;
            stream.container_index = channels == 1 ? 4 : 9;
            stream.codec.channels = channels;
            channel_metadata.streams.push_back(stream);
        }
        channel_session->addProbedMediaForHarness(QStringLiteral("source.mkv"), channel_metadata, true);
        engine->rootContext()->setContextProperty(QStringLiteral("testChannelSession"), channel_session);
        engine->rootContext()->setContextProperty(QStringLiteral("testSession"), session_);
        engine->rootContext()->setContextProperty(QStringLiteral("testTimeline"), timeline_);
        engine->rootContext()->setContextProperty(QStringLiteral("testPlayer"), player_);
        engine->rootContext()->setContextProperty(QStringLiteral("testTransportPlayer"), transport_player_);
        review_ = new exosnap::quick::RecordingReviewAdapter(this);
        engine->rootContext()->setContextProperty(QStringLiteral("testRecordingReview"), review_);

        auto* page_session = new EditSessionAdapter(this);
        page_session_ = page_session;
        auto* page_timeline = new EditTimelineAdapter(this);
        auto* page_player = new EditPlayerAdapter(this);
        page_player_ = page_player;
        auto* page_exporter = new exosnap::quick::EditExportAdapter(this);
        page_exporter_ = page_exporter;
        engine->rootContext()->setContextProperty(QStringLiteral("testEditHarness"), this);
        auto* page_recordings = new exosnap::quick::RecordViewModelAdapter(&record_view_model_, this);
        page_recordings_ = page_recordings;
        QObject::connect(page_recordings, &exosnap::quick::RecordViewModelAdapter::recordingRevealFinished, review_,
                         &exosnap::quick::RecordingReviewAdapter::completeEditRequest);
        page_exporter->setSession(page_session);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageSession"), page_session);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageTimeline"), page_timeline);
        engine->rootContext()->setContextProperty(QStringLiteral("testPagePlayer"), page_player);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageExporter"), page_exporter);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageRecordings"), page_recordings);
        page_shell_ = new exosnap::quick::ShellAdapter(this);
        engine->rootContext()->setContextProperty(QStringLiteral("testPageShell"), page_shell_);
    }

  private:
    QQmlEngine* visual_engine_ = nullptr;
    exosnap::quick::RecordingReviewAdapter* review_ = nullptr;
    QTemporaryDir review_visual_directory_;
    exosnap::RecordViewModel record_view_model_;
    exosnap::quick::RecordViewModelAdapter* page_recordings_ = nullptr;
    QString reviewed_recording_path_;
    EditSessionAdapter* session_ = nullptr;
    EditTimelineAdapter* timeline_ = nullptr;
    EditPlayerAdapter* player_ = nullptr;
    EditPlayerAdapter* transport_player_ = nullptr;
    EditPlayerAdapter* page_player_ = nullptr;
    EditSessionAdapter* page_session_ = nullptr;
    exosnap::quick::EditExportAdapter* page_exporter_ = nullptr;
    exosnap::quick::ShellAdapter* page_shell_ = nullptr;
    QMimeData held_mime_;
    bool held_drag_ = false;
};

QUICK_TEST_MAIN_WITH_SETUP(edit_timeline, Setup)

#include "edit_timeline_qml_test_main.moc"
