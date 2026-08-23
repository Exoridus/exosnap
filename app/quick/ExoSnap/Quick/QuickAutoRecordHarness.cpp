#include "QuickAutoRecordHarness.h"

#include "QuickApplication.h"
#include "RecordPreviewAdapter.h"

#include "benchmark/BenchmarkReport.h"
#include "models/VideoSettingsModel.h"
#include "services/RecordingCoordinator.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QQuickWindow>
#include <QTimer>

#include <windows.h>

#include <algorithm>

namespace exosnap::quick {
namespace {

// Same budget the Widgets preview path allows the asynchronous hardware
// capability query. It is a hardware probe, not a UI animation, so a slow
// machine legitimately needs seconds.
constexpr int kCoordinatorReadyTimeoutMs = 20000;

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
                               ? recorder_core::CaptureTarget::Kind::Window
                               : recorder_core::CaptureTarget::Kind::Monitor;
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
    hooks.onMeasurementStart = [adapter]() {
        if (adapter != nullptr)
            adapter->resetMetrics();
    };
    hooks.samplePreviewMetrics = [adapter, window]() {
        return adapter != nullptr ? SampleQuickPreviewMetrics(*adapter, window) : benchmark::PreviewMetrics{};
    };

    // Nothing here switches the QML overlay animation on. The Widgets frontend has
    // no equivalent, and an extra animated surface on one side only would be a
    // difference the report could not attribute to the frontends themselves.
    return auto_record::RunAutoRecordOnCoordinator(app, *coordinator, options, benchmark::Frontend::Quick, hooks,
                                                   out_last_outcome);
}

} // namespace exosnap::quick
