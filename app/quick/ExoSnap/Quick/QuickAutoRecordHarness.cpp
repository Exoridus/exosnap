#include "QuickAutoRecordHarness.h"

#include "QuickApplication.h"
#include "RecordPreviewAdapter.h"
#include "SettingsAdapter.h"

#include "benchmark/BenchmarkReport.h"
#include "diagnostics/NativeWindowFacts.h"
#include "models/VideoSettingsModel.h"
#include "observability/PipelineSnapshotJson.h"
#include "services/RecordingCoordinator.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSaveFile>
#include <QScreen>
#include <QTimer>

#include <windows.h>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>

namespace exosnap::quick {
namespace {

// Same budget the Widgets preview path allows the asynchronous hardware
// capability query. It is a hardware probe, not a UI animation, so a slow
// machine legitimately needs seconds.
constexpr int kCoordinatorReadyTimeoutMs = 20000;

qint64 PresentationQpc() {
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return ticks.QuadPart;
}

class OverlayPresentationProbe final : public QObject {
  public:
    OverlayPresentationProbe(QString variant, QString directory, OverlayTelemetryAdapter* telemetry)
        : variant_(std::move(variant)), directory_(std::move(directory)) {
        connect(telemetry, &OverlayTelemetryAdapter::snapshotChanged, this, [this] {
            if (start_qpc_ != 0 && !finished_)
                ++telemetry_publications_;
        });
        connect(&discover_, &QTimer::timeout, this, [this]() { discover(); });
        discover_.start(50);
    }

    void start() {
        discover();
        {
            const std::scoped_lock lock(counts_[0]->mutex, counts_[1]->mutex);
            start_qpc_ = PresentationQpc();
            for (auto& counts : counts_) {
                counts->renders = 0;
                counts->swaps = 0;
                counts->start_qpc = start_qpc_;
                counts->measuring = true;
            }
        }
        start_windows_ = windows();
        start_pipeline_ = pipeline();
        write(QStringLiteral("overlay-measurement-start.json"), QJsonObject{{QStringLiteral("startQpc"), start_qpc_}});
    }

    void finish() {
        if (start_qpc_ == 0 || finished_)
            return;
        finished_ = true;
        qint64 end_qpc = 0;
        std::array<quint64, 2> renders{};
        std::array<quint64, 2> swaps{};
        {
            const std::scoped_lock lock(counts_[0]->mutex, counts_[1]->mutex);
            end_qpc = PresentationQpc();
            for (size_t i = 0; i < counts_.size(); ++i) {
                counts_[i]->measuring = false;
                renders[i] = counts_[i]->renders;
                swaps[i] = counts_[i]->swaps;
            }
        }
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const double seconds = static_cast<double>(end_qpc - start_qpc_) / static_cast<double>(frequency.QuadPart);
        QJsonArray cadence;
        for (size_t i = 0; i < counts_.size(); ++i) {
            cadence.append(QJsonObject{{QStringLiteral("objectName"), names_[i]},
                                       {QStringLiteral("renders"), static_cast<qint64>(renders[i])},
                                       {QStringLiteral("frameSwapped"), static_cast<qint64>(swaps[i])},
                                       {QStringLiteral("rendersPerSecond"), renders[i] / seconds},
                                       {QStringLiteral("frameSwappedPerSecond"), swaps[i] / seconds}});
        }
        write(QStringLiteral("overlay-measurement.json"),
              QJsonObject{{QStringLiteral("variant"), variant_},
                          {QStringLiteral("pid"), QCoreApplication::applicationPid()},
                          {QStringLiteral("startQpc"), start_qpc_},
                          {QStringLiteral("endQpc"), end_qpc},
                          {QStringLiteral("qpcFrequency"), frequency.QuadPart},
                          {QStringLiteral("seconds"), seconds},
                          {QStringLiteral("telemetryPublications"), static_cast<qint64>(telemetry_publications_)},
                          {QStringLiteral("telemetryPublicationsPerSecond"), telemetry_publications_ / seconds},
                          {QStringLiteral("startWindows"), start_windows_},
                          {QStringLiteral("endWindows"), windows()},
                          {QStringLiteral("startPipeline"), start_pipeline_},
                          {QStringLiteral("endPipeline"), pipeline()},
                          {QStringLiteral("cadence"), cadence}});
    }

    bool wrote() const {
        return wrote_ && finished_;
    }
    void setCoordinator(RecordingCoordinator* coordinator) {
        coordinator_ = coordinator;
    }

  private:
    struct Counts {
        std::mutex mutex;
        bool measuring = false;
        qint64 start_qpc = 0;
        quint64 renders = 0;
        quint64 swaps = 0;
    };

    QJsonObject pipeline() const {
        exosnap::engine::RecordingDiagnosticsSnapshot snapshot;
        if (coordinator_ != nullptr && !coordinator_->LastDiagnosticsSnapshot(&snapshot))
            snapshot = {};
        QJsonObject result = observability::PipelineSnapshotToJson(snapshot);
        // The aggregate has no availability bit. Zero cannot distinguish absent
        // timestamp samples from zero work, so only positive samples are reported.
        result.insert(QStringLiteral("recorderGpuPassP99Ms"), snapshot.compositor.gpu_exec_p99_ms > 0.0
                                                                  ? QJsonValue(snapshot.compositor.gpu_exec_p99_ms)
                                                                  : QJsonValue(QJsonValue::Null));
        return result;
    }

    void discover() {
        for (QWindow* candidate : QGuiApplication::allWindows()) {
            for (size_t i = 0; i < names_.size(); ++i) {
                if (candidate->objectName() != names_[i] || windows_[i] != nullptr)
                    continue;
                auto* window = qobject_cast<QQuickWindow*>(candidate);
                if (window == nullptr)
                    continue;
                windows_[i] = window;
                // These signals originate on the render thread. Count there without
                // queued GUI work, and keep the counters alive through disconnection.
                const auto counts = counts_[i];
                connect(
                    window, &QQuickWindow::afterRendering, this,
                    [counts]() {
                        const std::lock_guard lock(counts->mutex);
                        if (counts->measuring && PresentationQpc() >= counts->start_qpc)
                            ++counts->renders;
                    },
                    Qt::DirectConnection);
                connect(
                    window, &QQuickWindow::frameSwapped, this,
                    [counts]() {
                        const std::lock_guard lock(counts->mutex);
                        if (counts->measuring && PresentationQpc() >= counts->start_qpc)
                            ++counts->swaps;
                    },
                    Qt::DirectConnection);
            }
        }
        if (windows_[0] != nullptr && windows_[1] != nullptr)
            discover_.stop();
    }

    QJsonArray windows() const {
        QJsonArray result;
        for (size_t i = 0; i < windows_.size(); ++i) {
            QQuickWindow* window = windows_[i];
            QJsonObject facts{{QStringLiteral("objectName"), names_[i]},
                              {QStringLiteral("instantiated"), window != nullptr}};
            if (window != nullptr) {
                facts.insert(QStringLiteral("visible"), window->isVisible());
                facts.insert(QStringLiteral("exposed"), window->isExposed());
                facts.insert(QStringLiteral("screen"),
                             window->screen() != nullptr ? window->screen()->name() : QString{});
                facts.insert(QStringLiteral("graphicsApi"),
                             static_cast<int>(window->rendererInterface()->graphicsApi()));
                if (window->handle() != nullptr) {
                    const auto native = diagnostics::QueryNativeWindowFacts(reinterpret_cast<void*>(window->winId()));
                    facts.insert(QStringLiteral("hwnd"), static_cast<qint64>(native.hwnd));
                    facts.insert(QStringLiteral("nativeValid"), native.valid);
                    facts.insert(QStringLiteral("nativeVisible"),
                                 IsWindowVisible(reinterpret_cast<HWND>(window->winId())) != FALSE);
                    facts.insert(QStringLiteral("layered"), native.layered);
                    facts.insert(QStringLiteral("transparentForInput"), native.transparent_for_input);
                    facts.insert(QStringLiteral("captureExcluded"),
                                 native.affinity_known && native.display_affinity == 0x11);
                    facts.insert(QStringLiteral("x"), native.x);
                    facts.insert(QStringLiteral("y"), native.y);
                    facts.insert(QStringLiteral("width"), native.width);
                    facts.insert(QStringLiteral("height"), native.height);
                }
                if (i == 0) {
                    facts.insert(QStringLiteral("overlayState"), window->property("overlayState").toInt());
                    facts.insert(QStringLiteral("diagnosticsActive"), window->property("diagnosticsActive").toBool());
                    for (const char* property :
                         {"showFps", "showDrop", "showDrift", "showDiagnosticsSize", "showMutedSources", "showHealth"})
                        facts.insert(QString::fromLatin1(property), window->property(property).toBool());
                }
            }
            result.append(facts);
        }
        return result;
    }

    void write(const QString& name, const QJsonObject& document) {
        QSaveFile file(QDir(directory_).filePath(name));
        const QByteArray bytes = QJsonDocument(document).toJson();
        const bool ok = QDir().mkpath(directory_) && file.open(QIODevice::WriteOnly) &&
                        file.write(bytes) == bytes.size() && file.commit();
        wrote_ = wrote_ && ok;
        if (!ok)
            qCritical().noquote() << QStringLiteral("overlay measurement write failed: %1").arg(file.fileName());
    }

    QString variant_;
    QString directory_;
    QTimer discover_;
    quint64 telemetry_publications_ = 0;
    qint64 start_qpc_ = 0;
    bool finished_ = false;
    bool wrote_ = true;
    QJsonArray start_windows_;
    QJsonObject start_pipeline_;
    RecordingCoordinator* coordinator_ = nullptr;
    const std::array<QString, 2> names_{QStringLiteral("quickOverlayRecording"),
                                        QStringLiteral("quickOverlayQuickControls")};
    std::array<QPointer<QQuickWindow>, 2> windows_;
    std::array<std::shared_ptr<Counts>, 2> counts_{std::make_shared<Counts>(), std::make_shared<Counts>()};
};

RecordingCoordinator* WaitForCoordinatorReady(QuickApplication& application, int timeout_ms, QString* error) {
    RecordingCoordinator* coordinator = application.recordingCoordinator();
    if (coordinator == nullptr) {
        *error = QStringLiteral("the Quick composition owner has no recording coordinator");
        return nullptr;
    }

    QElapsedTimer clock;
    clock.start();
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(25);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (coordinator->State() == UiRecordingState::Ready || clock.elapsed() >= timeout_ms)
            loop.quit();
    });
    poll.start();
    loop.exec();
    poll.stop();

    if (coordinator->State() != UiRecordingState::Ready) {
        *error = QStringLiteral("the coordinator never reached Ready (%1)")
                     .arg(QString::fromStdWString(coordinator->CapabilityStatusText()));
        return nullptr;
    }
    return coordinator;
}

benchmark::PreviewMetrics SampleQuickPreviewMetrics(const RecordPreviewAdapter& adapter, QQuickWindow* window) {
    const PreviewMetricsSnapshot metrics = adapter.previewMetricsSnapshot();
    constexpr auto kSame = benchmark::Comparability::Identical;
    constexpr auto kApprox = benchmark::Comparability::Approximate;

    benchmark::PreviewMetrics preview;
    preview.frames_presented =
        benchmark::MakeMetric(static_cast<double>(metrics.render_frames), kApprox,
                              "ExoPreviewItem: scene-graph renders of the WHOLE window on Qt's render "
                              "thread, not of the preview quad alone");
    preview.source_frames_consumed =
        benchmark::MakeMetric(static_cast<double>(metrics.consumed_frames), kSame,
                              "ExoPreviewItem: frames taken off the shared preview texture");
    preview.mutex_misses = benchmark::MakeMetric(
        static_cast<double>(metrics.mutex_misses), kApprox,
        "ExoPreviewItem: keyed-mutex AcquireSync(0) failures, one per attempt — the scene graph asks once "
        "per rendered frame, and renders are now requested per published frame rather than per vsync");
    preview.frame_cadence_fps =
        benchmark::MakeMetric(metrics.scene_fps, kApprox, "ExoPreviewItem: scene-graph render rate");
    preview.frame_ms_p50 =
        benchmark::MakeMetric(metrics.scene_frame_ms_p50, kApprox, "ExoPreviewItem: inter-render interval");
    preview.frame_ms_p95 =
        benchmark::MakeMetric(metrics.scene_frame_ms_p95, kApprox, "ExoPreviewItem: inter-render interval");
    preview.frame_ms_p99 =
        benchmark::MakeMetric(metrics.scene_frame_ms_p99, kApprox, "ExoPreviewItem: inter-render interval");
    preview.frame_ms_max =
        benchmark::MakeMetric(metrics.scene_frame_ms_max, kApprox, "ExoPreviewItem: inter-render interval");
    preview.source_delivery_fps =
        benchmark::MakeMetric(metrics.source_delivery_fps, kApprox,
                              "ExoPreviewItem: rate at which frames arrived AT THIS CONSUMER; observed on the consume "
                              "side, so it is what the preview got and never what the engine produced");
    preview.source_interval_ms_p95 = benchmark::MakeMetric(metrics.source_interval_ms_p95, kApprox,
                                                           "ExoPreviewItem: consumer-observed arrival interval");
    preview.source_interval_ms_p99 = benchmark::MakeMetric(metrics.source_interval_ms_p99, kApprox,
                                                           "ExoPreviewItem: consumer-observed arrival interval");
    preview.submit_us_p50 =
        benchmark::MakeMetric(metrics.submit_us_p50, kSame, "ExoPreviewItem: GPU submit for the preview copy");
    preview.submit_us_p95 =
        benchmark::MakeMetric(metrics.submit_us_p95, kSame, "ExoPreviewItem: GPU submit for the preview copy");
    preview.submit_us_p99 =
        benchmark::MakeMetric(metrics.submit_us_p99, kSame, "ExoPreviewItem: GPU submit for the preview copy");
    preview.child_hwnd_count = benchmark::MakeMetric(
        static_cast<double>(
            benchmark::CountChildWindows(window != nullptr ? reinterpret_cast<void*>(window->winId()) : nullptr)),
        benchmark::Comparability::FrontendOnly,
        "EnumChildWindows over the top-level HWND; the Quick preview is a scene-graph item, so zero is "
        "the expected result and the point of the migration");
    if (metrics.consumed_frames > 0) {
        preview.render_amplification = benchmark::MakeMetric(
            static_cast<double>(metrics.render_frames) / static_cast<double>(metrics.consumed_frames), kApprox,
            "ExoPreviewItem: whole-window scene-graph renders per frame taken off the shared texture");
    } else {
        preview.render_amplification =
            benchmark::UnavailableMetric(kApprox, "ExoPreviewItem: no source frame was consumed in the window");
    }
    preview.preview_publish_signals = benchmark::MakeMetric(
        static_cast<double>(metrics.publish_signals), benchmark::Comparability::FrontendOnly,
        "PreviewUpdateScheduler: per-frame publish edges the producers emitted (capture hub or engine tap)");
    preview.preview_scene_update_requests = benchmark::MakeMetric(
        static_cast<double>(metrics.scene_update_requests), benchmark::Comparability::FrontendOnly,
        "PreviewUpdateScheduler: wake-ups that reached the live item as one QQuickItem::update()");
    preview.consumer_acquires = benchmark::MakeMetric(
        static_cast<double>(metrics.acquires), kSame,
        "ExoPreviewItem: keyed-mutex acquires that succeeded — the frame left the slot, whether or not it "
        "then converted");
    preview.consumer_acquire_abandoned = benchmark::MakeMetric(
        static_cast<double>(metrics.acquire_abandoned), kSame,
        "ExoPreviewItem: acquires that found the keyed mutex abandoned — the shared surface is inconsistent");
    preview.publish_interval_ms_p50 = benchmark::MakeMetric(metrics.publish_interval_ms_p50, kSame,
                                                            "PreviewUpdateScheduler: interval between successful "
                                                            "publishes, measured at the publish edge");
    preview.publish_interval_ms_p95 =
        benchmark::MakeMetric(metrics.publish_interval_ms_p95, kSame, "PreviewUpdateScheduler: publish interval");
    preview.publish_interval_ms_p99 =
        benchmark::MakeMetric(metrics.publish_interval_ms_p99, kSame, "PreviewUpdateScheduler: publish interval");
    preview.publish_interval_ms_max =
        benchmark::MakeMetric(metrics.publish_interval_ms_max, kSame, "PreviewUpdateScheduler: publish interval");
    preview.presentation_debt_ms_p50 =
        benchmark::MakeMetric(metrics.debt_age_ms_p50, kApprox,
                              "PreviewUpdateScheduler: how long the preview owed a frame, from the first publish "
                              "after the last successful consume to that consume");
    preview.presentation_debt_ms_p95 =
        benchmark::MakeMetric(metrics.debt_age_ms_p95, kApprox, "PreviewUpdateScheduler: presentation debt age");
    preview.presentation_debt_ms_p99 =
        benchmark::MakeMetric(metrics.debt_age_ms_p99, kApprox, "PreviewUpdateScheduler: presentation debt age");
    preview.presentation_debt_ms_max =
        benchmark::MakeMetric(metrics.debt_age_ms_max, kApprox, "PreviewUpdateScheduler: presentation debt age");
    preview.source_interval_ms_max = benchmark::MakeMetric(
        metrics.source_interval_ms_max, kApprox, "ExoPreviewItem: worst consumer-observed arrival gap in the window");
    preview.consumer_conversion_failures = benchmark::MakeMetric(
        static_cast<double>(metrics.conversion_failures), kSame,
        "ExoPreviewItem: frames taken off the slot that then failed tone-map/RGBA conversion and can never be "
        "taken again");
    preview.consumer_release_failures = benchmark::MakeMetric(
        static_cast<double>(metrics.release_failures), kSame,
        "ExoPreviewItem: consumed frames whose keyed-mutex release failed -- the producer can never take the "
        "mutex again, so every mutex_miss after this is a dead transport, not contention");
    return preview;
}

// The idle-preview snapshot. Deliberately routed through
// RecordingCoordinator::CaptureFrame() rather than assembling a
// ReadyFrameComposition here: the composition carries the crop, the video
// settings and the webcam overlay, and a harness that built its own would be
// measuring a picture the product never produces.
//
// What this exercises that `capture_frame_at_seconds` cannot: the Ready branch
// goes through ReadyFrameCaptureService, which applies the SAME PreviewTapDesc
// transform ExoPreviewItem applies. Its output is therefore the preview's own
// colour pipeline, readable as a file and comparable against a frame decoded
// from a recording of the same desktop -- which is the only automated way to
// tell a preview-side HDR/SDR mistake from an engine-side one.
int CaptureReadyFrame(QCoreApplication& app, RecordingCoordinator& coordinator, RecordPreviewAdapter* adapter,
                      const auto_record::AutoRecordOptions& options) {
    constexpr int kFrameReadyTimeoutMs = 15000;
    constexpr int kCaptureTimeoutMs = 15000;

    if (adapter == nullptr) {
        qCritical().noquote() << QStringLiteral("auto-record: no preview adapter for --capture-frame-in-ready");
        return 1;
    }
    QElapsedTimer clock;
    clock.start();
    {
        QEventLoop wait_ready;
        QTimer poll;
        poll.setInterval(25);
        QObject::connect(&poll, &QTimer::timeout, &wait_ready, [&]() {
            if (adapter->frameReady() || clock.elapsed() >= kFrameReadyTimeoutMs)
                wait_ready.quit();
        });
        poll.start();
        wait_ready.exec();
    }
    if (!adapter->frameReady()) {
        qCritical().noquote() << QStringLiteral("auto-record: the idle preview never produced a frame in %1 ms")
                                     .arg(kFrameReadyTimeoutMs);
        return 3;
    }

    // Clobbers the application's own frame-captured handler (a toast). That is
    // sound only because this mode reports one line and exits without ever
    // starting a recording; it must not be copied into a mode that keeps running.
    bool done = false;
    bool ok = false;
    QString written_path;
    QString capture_error;
    QEventLoop wait_capture;
    coordinator.SetFrameCapturedCallback([&](bool success, const QString& path, const QString& error) {
        done = true;
        ok = success;
        written_path = path;
        capture_error = error;
        wait_capture.quit();
    });

    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &wait_capture, [&]() { wait_capture.quit(); });
    deadline.start(kCaptureTimeoutMs);

    coordinator.CaptureFrame();
    wait_capture.exec();
    deadline.stop();

    if (!done) {
        qCritical().noquote()
            << QStringLiteral("auto-record: the Ready snapshot did not complete in %1 ms").arg(kCaptureTimeoutMs);
        return 3;
    }
    if (!ok) {
        qCritical().noquote() << QStringLiteral("auto-record: the Ready snapshot failed: %1").arg(capture_error);
        return 1;
    }

    // The engine names and places the file. An explicit --screenshot-path is
    // honoured by MOVING it afterwards rather than by teaching the engine a
    // second naming rule: the path the product would have produced stays the one
    // that was produced.
    if (!options.screenshot_path.isEmpty() && !written_path.isEmpty()) {
        const QString destination = QDir::toNativeSeparators(options.screenshot_path);
        QFile::remove(destination);
        if (!QFile::rename(written_path, destination)) {
            qCritical().noquote() << QStringLiteral(
                                         "auto-record: could not move the Ready snapshot to %1 (it is at %2)")
                                         .arg(destination, written_path);
            return 2;
        }
        written_path = destination;
    }

    qInfo().noquote() << QStringLiteral("auto-record-ready-frame: ok=1 path=%1").arg(written_path);
    Q_UNUSED(app);
    return 0;
}

} // namespace

int RunQuickAutoRecord(QCoreApplication& app, QuickApplication& application, QQuickWindow* window,
                       const auto_record::AutoRecordOptions& options, benchmark::RunOutcome* out_last_outcome) {
    const QString variant = qEnvironmentVariable("EXOSNAP_OVERLAY_PRESENTATION_VARIANT");
    std::unique_ptr<OverlayPresentationProbe> overlay_probe;
    if (!variant.isEmpty()) {
        const QStringList variants{QStringLiteral("off"),        QStringLiteral("hud-minimal"),
                                   QStringLiteral("hud-health"), QStringLiteral("hud-full"),
                                   QStringLiteral("dock-only"),  QStringLiteral("hud-full-dock")};
        if (!variants.contains(variant) || options.benchmark_scenario.isEmpty() ||
            options.benchmark_output_dir.isEmpty() || options.repeat_cycles != 1 || options.pause_at_seconds >= 0 ||
            qEnvironmentVariableIsEmpty("EXOSNAP_CONFIG_DIR")) {
            qCritical(
                "overlay presentation verification requires a valid variant, isolated configuration, one unpaused "
                "benchmark cycle and an output directory");
            return 1;
        }
        SettingsAdapter* settings = application.settingsAdapter();
        settings->setShowRecordingOverlay(variant.startsWith(QStringLiteral("hud-")));
        settings->setShowQuickControls(variant == QStringLiteral("dock-only") ||
                                       variant.endsWith(QStringLiteral("-dock")));
        const QString hud_variant = variant.endsWith(QStringLiteral("-dock")) ? variant.chopped(5) : variant;
        settings->setShowDiagnosticsOverlay(hud_variant == QStringLiteral("hud-full") ||
                                            variant == QStringLiteral("hud-health"));
        settings->setShowNotifications(false);
        settings->setRecordingOverlayPreset(QStringLiteral("minimal"));
        if (hud_variant == QStringLiteral("hud-full")) {
            settings->setDiagnosticsOverlayPreset(QStringLiteral("technical"));
        } else if (variant == QStringLiteral("hud-health")) {
            settings->setDiagnosticsOverlayPreset(QStringLiteral("health"));
        }
        overlay_probe = std::make_unique<OverlayPresentationProbe>(variant, options.benchmark_output_dir,
                                                                   application.overlayTelemetryAdapter());
    }
    // Same rule as the Widgets side, resolved by the same function: visible, on
    // the secondary screen when there is one, at one shared logical size.
    if (window != nullptr) {
        const benchmark::HarnessWindowPlacement placement = benchmark::ResolveHarnessWindowPlacement();
        if (!placement.screen_name.isEmpty())
            window->setGeometry(placement.x, placement.y, placement.width, placement.height);
        else
            window->resize(placement.width, placement.height);
    }

    QString wait_error;
    RecordingCoordinator* coordinator = WaitForCoordinatorReady(application, kCoordinatorReadyTimeoutMs, &wait_error);
    if (coordinator == nullptr) {
        qCritical().noquote() << QStringLiteral("auto-record: %1").arg(wait_error);
        return 1;
    }
    if (overlay_probe != nullptr)
        overlay_probe->setCoordinator(coordinator);

    // Put the IDLE preview on the requested frame rate before the target is
    // selected, mirroring RecordPage::setVideoSettings on the Widgets side.
    // Deliberately only the video settings: the output format is committed by the
    // shared drive loop from the CLI options, and seeding a second, defaulted
    // OutputSettingsModel here is exactly the defect that made an earlier
    // Widgets-vs-Quick pair incomparable.
    VideoSettingsModel preview_video_settings = VideoSettingsModel::Defaults();
    preview_video_settings.frame_rate_num = static_cast<uint32_t>(std::clamp(options.frame_rate, 1, 240));
    preview_video_settings.frame_rate_den = 1;
    coordinator->SetVideoSettings(preview_video_settings);

    const auto want_kind = options.target == auto_record::TargetKind::Window
                               ? exosnap::engine::CaptureTarget::Kind::Window
                               : exosnap::engine::CaptureTarget::Kind::Monitor;
    if (!application.selectCaptureTargetForAutomation(want_kind, options.target_window_title)) {
        const QString what = options.target == auto_record::TargetKind::Monitor
                                 ? QStringLiteral("monitor")
                                 : QStringLiteral("window matching \"%1\"").arg(options.target_window_title);
        qCritical().noquote() << QStringLiteral("auto-record: no matching capture target (%1)").arg(what);
        return 1;
    }

    RecordPreviewAdapter* adapter = application.recordPreviewAdapter();

    // Before any recording is started, and it never starts one: this mode exists
    // to photograph the IDLE preview's own transform.
    if (options.capture_frame_in_ready)
        return CaptureReadyFrame(app, *coordinator, adapter, options);

    auto_record::BenchmarkHooks hooks;
    hooks.onMeasurementStart = [adapter, &overlay_probe]() {
        if (adapter != nullptr)
            adapter->resetMetrics();
        if (overlay_probe != nullptr)
            overlay_probe->start();
    };
    hooks.onMeasurementEnd = [&overlay_probe]() {
        if (overlay_probe != nullptr)
            overlay_probe->finish();
    };
    hooks.samplePreviewMetrics = [adapter, window]() {
        return adapter != nullptr ? SampleQuickPreviewMetrics(*adapter, window) : benchmark::PreviewMetrics{};
    };

    const int result = auto_record::RunAutoRecordOnCoordinator(app, *coordinator, options, benchmark::Frontend::Quick,
                                                               hooks, out_last_outcome);
    if (overlay_probe != nullptr) {
        overlay_probe->finish();
        if (!overlay_probe->wrote())
            return 1;
    }
    return result;
}

} // namespace exosnap::quick
