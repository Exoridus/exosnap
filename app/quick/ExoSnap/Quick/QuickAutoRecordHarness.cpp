#include "QuickAutoRecordHarness.h"

#include "QuickApplication.h"
#include "RecordPreviewAdapter.h"

#include "benchmark/BenchmarkReport.h"
#include "models/VideoSettingsModel.h"
#include "services/RecordingCoordinator.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
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
    return preview;
}

} // namespace

int RunQuickAutoRecord(QCoreApplication& app, QuickApplication& application, QQuickWindow* window,
                       const auto_record::AutoRecordOptions& options) {
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
    return auto_record::RunAutoRecordOnCoordinator(app, *coordinator, options, benchmark::Frontend::Quick, hooks);
}

} // namespace exosnap::quick
