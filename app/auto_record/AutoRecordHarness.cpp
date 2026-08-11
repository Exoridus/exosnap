#include "AutoRecordHarness.h"

#include <filesystem>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

#include "../benchmark/BenchmarkEngineMetrics.h"

#include <capability/audio_ui_state.h>
#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <recorder_core/audio_track_model.h>
#include <recorder_core/codec_types.h>
#include <recorder_core/recorder_session.h>

#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../services/RecordingCoordinator.h"

// Everything above this line is Qt Widgets-free on purpose: RunAutoRecordOnCoordinator
// is the single orchestration path for BOTH frontends, so the Qt Quick target compiles
// this translation unit too (with EXOSNAP_ENABLE_VISUAL_TEST_HARNESS off, which excludes
// the Widgets-bound preview entry point below). A stray <QApplication> or MainWindow
// include out here would drag Qt6::Widgets back into the Quick binary and undo the very
// property the cutover is about.
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QScreen>

#include "../MainWindow.h"
#include "../diagnostics/AppLog.h"
#include "../pages/RecordPage.h"
#include "../services/DxgiPreviewRenderer.h"
#include "../viewmodels/RecordViewModel.h"
#include "../visual_tests/VisualTestHarness.h"
#endif

// HasAutoRecordRequest / ParseAutoRecordOptions live in AutoRecordOptions.cpp — pure
// parsing with no Widgets/RecordingCoordinator dependency, so the parser's gtest
// target can link Qt6::Core only. This file holds the recording drive loop.

namespace exosnap::auto_record {
namespace {

// ---------------------------------------------------------------------------
// Option → engine-type mapping
//
// These mirror the field-for-field translation the Output settings model does from
// live UI state; here the "UI state" is the parsed AutoRecordOptions instead. The
// CLI parser (above) already restricts the
// string/int inputs to the valid sets, so the defaults below are only reached for
// values the parser passes through unchecked (--video-codec / --audio-codec).
// ---------------------------------------------------------------------------

capability::Container MapContainer(const QString& name) {
    if (name == QStringLiteral("mp4"))
        return capability::Container::Mp4;
    if (name == QStringLiteral("webm"))
        return capability::Container::WebM;
    return capability::Container::Matroska; // "mkv"
}

capability::VideoCodec MapVideoCodec(const QString& name) {
    if (name == QStringLiteral("h264"))
        return capability::VideoCodec::H264;
    if (name == QStringLiteral("hevc"))
        return capability::VideoCodec::Hevc;
    return capability::VideoCodec::Av1; // "av1"
}

capability::AudioCodec MapAudioCodec(const QString& name) {
    if (name == QStringLiteral("aac"))
        return capability::AudioCodec::Aac;
    if (name == QStringLiteral("pcm"))
        return capability::AudioCodec::Pcm;
    if (name == QStringLiteral("flac"))
        return capability::AudioCodec::Flac;
    return capability::AudioCodec::Opus; // "opus"
}

capability::ChromaSubsampling MapChroma(int chroma) {
    return chroma == 444 ? capability::ChromaSubsampling::Cs444 : capability::ChromaSubsampling::Cs420;
}

capability::BitDepth MapBitDepth(int bit_depth) {
    return bit_depth == 10 ? capability::BitDepth::Bit10 : capability::BitDepth::Bit8;
}

recorder_core::HdrMode MapHdrMode(HdrMode mode) {
    switch (mode) {
    case HdrMode::Tonemap:
        return recorder_core::HdrMode::TonemapSdr;
    case HdrMode::Native:
        return recorder_core::HdrMode::Hdr10;
    case HdrMode::Off:
    default:
        return recorder_core::HdrMode::Off;
    }
}

recorder_core::AudioSourceKind RowKindFromName(const QString& name) {
    if (name == QStringLiteral("app"))
        return recorder_core::AudioSourceKind::App;
    if (name == QStringLiteral("mic"))
        return recorder_core::AudioSourceKind::Mic;
    // "sys": keep the process-scoped Sys kind. BuildAudioPlan() normalizes Sys → the
    // pid-free SystemOutput for a Display/Region target (NormalizeSourceRowsForTarget),
    // so this is correct for every target kind without special-casing here.
    return recorder_core::AudioSourceKind::Sys;
}

OutputSettingsModel BuildOutputSettings(const AutoRecordOptions& options) {
    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    // Fallback output folder; the harness normally sets EXOSNAP_OUTPUT_DIR, which
    // overrides this at recording time (RecordingCoordinator::EffectiveOutputFolder).
    settings.output_folder = std::filesystem::temp_directory_path();
    settings.container = MapContainer(options.container);
    settings.video_codec = MapVideoCodec(options.video_codec);
    settings.audio_codec = MapAudioCodec(options.audio_codec);
    settings.bit_depth = MapBitDepth(options.bit_depth);
    settings.chroma_subsampling = MapChroma(options.chroma);
    settings.hdr_mode = MapHdrMode(options.hdr_mode);
    return settings;
}

capability::AudioUiState BuildAudioUiState(const AutoRecordOptions& options) {
    capability::AudioUiState state;
    state.target_kind = (options.target == TargetKind::Window) ? capability::CaptureTargetKind::Window
                                                               : capability::CaptureTargetKind::Display;
    for (const QString& row_name : options.audio_rows) {
        recorder_core::AudioSourceRow row;
        row.kind = RowKindFromName(row_name);
        row.enabled = true;
        row.merge_with_above = (!options.merge_above.isEmpty() && row_name == options.merge_above);
        state.source_rows.push_back(row);
    }
    return state;
}

QJsonObject ResultToJson(bool ok, const QString& output_path, const QString& session_report_path,
                         const QString& error_detail) {
    QJsonObject obj;
    obj.insert(QStringLiteral("status"), ok ? QStringLiteral("ok") : QStringLiteral("error"));
    obj.insert(QStringLiteral("output_path"), output_path);
    obj.insert(QStringLiteral("session_report_path"), session_report_path);
    obj.insert(QStringLiteral("error_detail"), error_detail);
    return obj;
}

void PrintResultLine(const QJsonObject& obj) {
    QTextStream(stdout) << QJsonDocument(obj).toJson(QJsonDocument::Compact) << Qt::endl;
}

int FailWith(const QString& error_detail) {
    PrintResultLine(ResultToJson(false, QString(), QString(), error_detail));
    return 1;
}

// Shared between RunAutoRecordOnCoordinator and the preview-mode RunAutoRecord entry
// point below — both reject a Region target with the identical message.
QString RegionNotSupportedError() {
    return QStringLiteral("region target is not supported by --auto-record yet");
}

[[nodiscard]] bool BenchmarkModeRequested(const AutoRecordOptions& options) {
    return !options.benchmark_scenario.trimmed().isEmpty();
}

// Mirrors the CLI options into the report's recording configuration verbatim, so a
// reader never has to reconstruct what was actually recorded from the flags used.
benchmark::RunConfig BuildBenchmarkRunConfig(const AutoRecordOptions& options, benchmark::Frontend frontend,
                                             int run_index, const recorder_core::CaptureTarget& target) {
    benchmark::RunConfig config;
    config.frontend = frontend;
    config.scenario = options.benchmark_scenario.trimmed();
    config.run_index = run_index;
    config.repetitions = options.repeat_cycles;
    config.warmup_seconds = options.benchmark_warmup_seconds;
    config.measured_seconds = options.duration_seconds;
    config.capture_target_kind =
        options.target == TargetKind::Window ? QStringLiteral("window") : QStringLiteral("monitor");
    config.capture_target_description = QString::fromStdString(target.description);
    config.requested_fps = options.frame_rate;
    config.container = options.container;
    config.video_codec = options.video_codec;
    config.audio_codec = options.audio_codec;
    config.chroma = options.chroma;
    config.bit_depth = options.bit_depth;
    switch (options.hdr_mode) {
    case HdrMode::Tonemap:
        config.hdr_mode = QStringLiteral("tonemap");
        break;
    case HdrMode::Native:
        config.hdr_mode = QStringLiteral("native");
        break;
    case HdrMode::Off:
    default:
        config.hdr_mode = QStringLiteral("off");
        break;
    }
    config.audio_rows = options.audio_rows.join(QLatin1Char(','));
    config.source_notes = options.benchmark_source_notes;
    return config;
}

} // namespace

int RunAutoRecordOnCoordinator(QCoreApplication& app, exosnap::RecordingCoordinator& coordinator,
                               const AutoRecordOptions& options, benchmark::Frontend frontend,
                               const BenchmarkHooks& hooks) {
    if (options.target == TargetKind::Region) {
        // Region capture (Monitor + crop_region) is out of the v1 checklist scope for
        // the harness; reject it honestly rather than silently recording the full monitor.
        return FailWith(RegionNotSupportedError());
    }

    // Synchronous capability probe. Bare mode has no UI responsiveness constraint, so
    // it skips the worker-thread hop MainWindow uses (app/MainWindow.cpp:1093-1109).
    const capability::CapabilitySet caps = capability::CapabilityBuilder::BuildFromHardwareQuery();

    // Commit the requested format BEFORE the caps gate, mirroring
    // RecordPage::initCoordinator() (SetOutputSettings/SetVideoSettings always run before
    // OnCapabilitiesReady there too). OnCapabilitiesReady validates whatever is already
    // applied — it must never be handed a validation computed against a different config,
    // or that config silently overwrites the one just committed here the moment caps land
    // (the exact bug this harness used to reproduce without ever catching it: it built its
    // own separate seed config instead of committing first).
    coordinator.SetOutputSettings(BuildOutputSettings(options));
    // The frame rate has to travel through the video settings: RecordingCoordinator
    // stamps the recording config from video_settings, so a rate set anywhere else is
    // silently replaced by 60.
    VideoSettingsModel video_settings = VideoSettingsModel::Defaults();
    video_settings.frame_rate_num = static_cast<uint32_t>(options.frame_rate);
    video_settings.frame_rate_den = 1;
    coordinator.SetVideoSettings(video_settings);

    coordinator.OnCapabilitiesReady(caps);

    if (coordinator.State() != UiRecordingState::Ready) {
        return FailWith(QStringLiteral("recording unavailable: %1")
                            .arg(QString::fromStdWString(coordinator.CapabilityStatusText())));
    }

    // Select a capture target.
    //   Monitor → the first display-kind target.
    //   Window  → the first window-kind target whose description contains the requested title.
    const std::vector<recorder_core::CaptureTarget> targets = coordinator.EnumerateTargets();
    recorder_core::CaptureTarget selected_target;
    bool found_target = false;
    for (const auto& target : targets) {
        if (options.target == TargetKind::Monitor) {
            if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
                selected_target = target;
                found_target = true;
                break;
            }
        } else { // TargetKind::Window
            if (target.kind == recorder_core::CaptureTarget::Kind::Window &&
                QString::fromStdString(target.description).contains(options.target_window_title, Qt::CaseInsensitive)) {
                selected_target = target;
                found_target = true;
                break;
            }
        }
    }
    if (!found_target) {
        const QString what = options.target == TargetKind::Monitor
                                 ? QStringLiteral("monitor")
                                 : QStringLiteral("window matching \"%1\"").arg(options.target_window_title);
        return FailWith(QStringLiteral("no matching capture target (%1)").arg(what));
    }

    const capability::AudioUiState audio_state = BuildAudioUiState(options);

    // repeat_cycles > 1 drives N start/stop cycles on this same coordinator instance
    // instead of exiting after one, so the capture hub sees the "warm" (already-leased,
    // reopen-on-next-start) state a real recording after a prior one in the same running
    // app session has — a cold-process single cycle cannot exercise that path. One JSON
    // result line per cycle; the process exit code reflects whether every cycle succeeded.
    const bool benchmark_mode = BenchmarkModeRequested(options);
    // Collected once: none of it changes between cycles, and probing DXGI/the
    // registry inside the measured window would be measuring the harness.
    const benchmark::Environment environment =
        benchmark_mode ? benchmark::CollectEnvironment() : benchmark::Environment{};

    bool all_ok = true;
    for (int cycle = 0; cycle < options.repeat_cycles; ++cycle) {
        // Result is delivered via callback, posted to this (Qt main) thread by the
        // coordinator — possibly asynchronously after a background remux for MP4. Capture
        // it and quit the loop as soon as it lands; the safety timer below is only a
        // fallback for a hang.
        QString final_output_path;
        QString final_error;
        bool have_result = false;
        bool succeeded = false;
        benchmark::RunOutcome outcome;
        coordinator.SetResultReadyCallback([&](const UiRecordingResult& result) {
            have_result = true;
            succeeded = result.succeeded;
            final_output_path = QString::fromStdWString(result.output_path);
            final_error = QString::fromStdWString(result.error_detail);
            outcome.succeeded = result.succeeded;
            outcome.output_path = final_output_path;
            outcome.output_file_bytes = static_cast<qint64>(result.output_file_bytes);
            outcome.media_duration_seconds = result.media_duration_seconds;
            outcome.output_width = static_cast<int>(result.output_width);
            outcome.output_height = static_cast<int>(result.output_height);
            outcome.error_phase = QString::fromStdWString(result.error_phase);
            outcome.error_detail = final_error;
            app.quit();
        });

        // Benchmark mode only. Sampling starts when the warm-up ends, not when the
        // process does, so start-up cost never lands in the measured window.
        benchmark::ProcessSampler process_sampler;
        bool measurement_started = false;
        // Engine counters at the instant the measured window opens. Everything the
        // engine reports is cumulative for the session, so without this the report's
        // frame and drop counts would include the warm-up — the interval the warm-up
        // exists to keep out of the measurement.
        recorder_core::RecordingDiagnosticsSnapshot baseline_snapshot;

        if (!coordinator.StartRecording(selected_target, audio_state)) {
            return FailWith(
                QStringLiteral("StartRecording refused (coordinator not ready or busy, cycle %1)").arg(cycle + 1));
        }

        // Stack-local (not &app-parented) singleShot timers: a timer bound to the
        // long-lived &app across --repeat-cycles iterations would keep counting
        // down after its own cycle's app.exec() returns and could fire during a
        // later cycle's event loop. Stopped explicitly below before the next
        // cycle starts.
        QTimer captureFrameTimer;
        captureFrameTimer.setSingleShot(true);
        if (options.capture_frame_at_seconds > 0) {
            // Optional mid-recording frame capture (PNG to the output folder).
            QObject::connect(&captureFrameTimer, &QTimer::timeout, &app,
                             [&coordinator]() { coordinator.CaptureFrame(); });
            captureFrameTimer.start(options.capture_frame_at_seconds * 1000);
        }

        // Stop after the requested duration.
        QTimer stopTimer;
        stopTimer.setSingleShot(true);
        QObject::connect(&stopTimer, &QTimer::timeout, &app, [&coordinator]() { coordinator.StopRecording(); });

        // Optional pause/resume inside the run. A paused recording must keep
        // producing the requested amount of MEDIA, so the paused interval is
        // added to the stop deadline rather than eaten out of it — otherwise a
        // pause that silently kept recording and one that silently dropped
        // frames would both produce a plausible-looking file.
        const int pause_budget_seconds = options.pause_at_seconds >= 0 ? std::max(0, options.pause_for_seconds) : 0;
        QTimer pauseTimer;
        QTimer resumeTimer;
        pauseTimer.setSingleShot(true);
        resumeTimer.setSingleShot(true);
        if (options.pause_at_seconds >= 0) {
            QObject::connect(&pauseTimer, &QTimer::timeout, &app, [&coordinator]() { coordinator.PauseRecording(); });
            QObject::connect(&resumeTimer, &QTimer::timeout, &app, [&coordinator]() { coordinator.ResumeRecording(); });
            pauseTimer.start(options.pause_at_seconds * 1000);
            resumeTimer.start((options.pause_at_seconds + pause_budget_seconds) * 1000);
        }

        // Benchmark mode inserts a warm-up *inside* the recording: the measured
        // window opens only once the coordinator has actually reached Recording and
        // the warm-up has elapsed on top of that. Without this the first seconds of
        // every run measure encoder spin-up, first-frame allocation and swap-chain
        // creation rather than steady-state cost — and would do so differently on
        // the two frontends, which is exactly the comparison this must not corrupt.
        //
        // Every timer here is stack-local for the same reason the ones above are: a
        // timer parented to the long-lived `app` would keep counting across cycles
        // and could fire into a later cycle's event loop with dangling references.
        QTimer warmupTimer;
        warmupTimer.setSingleShot(true);
        QObject::connect(&warmupTimer, &QTimer::timeout, &app, [&]() {
            if (hooks.onMeasurementStart)
                hooks.onMeasurementStart();
            if (!coordinator.LastDiagnosticsSnapshot(&baseline_snapshot))
                baseline_snapshot = recorder_core::RecordingDiagnosticsSnapshot{};
            process_sampler.Start();
            measurement_started = true;
            stopTimer.start((options.duration_seconds + pause_budget_seconds) * 1000);
        });

        QTimer recordingPoll;
        if (benchmark_mode) {
            recordingPoll.setInterval(5);
            QObject::connect(&recordingPoll, &QTimer::timeout, &app, [&]() {
                if (coordinator.State() != UiRecordingState::Recording)
                    return;
                recordingPoll.stop();
                warmupTimer.start(options.benchmark_warmup_seconds * 1000);
            });
            recordingPoll.start();
        } else {
            stopTimer.start((options.duration_seconds + pause_budget_seconds) * 1000);
        }

        // Safety net: if the result never arrives (hang, crash-in-teardown), quit anyway so
        // the process exits with a failure rather than blocking forever. Generous enough to
        // cover finalize + an MP4 remux of a short clip.
        constexpr int kGraceMs = 30000;
        const int wall_clock_ms =
            (options.duration_seconds + options.benchmark_warmup_seconds + pause_budget_seconds) * 1000;
        QTimer graceTimer;
        graceTimer.setSingleShot(true);
        QObject::connect(&graceTimer, &QTimer::timeout, &app, [&app]() { app.quit(); });
        graceTimer.start(wall_clock_ms + kGraceMs);

        app.exec();

        // None of these may survive into the next cycle's app.exec().
        captureFrameTimer.stop();
        stopTimer.stop();
        pauseTimer.stop();
        resumeTimer.stop();
        warmupTimer.stop();
        recordingPoll.stop();
        graceTimer.stop();

        bool ok = have_result && succeeded && final_error.isEmpty() && !final_output_path.isEmpty();
        if (!ok && final_error.isEmpty()) {
            final_error = have_result ? QStringLiteral("recording did not produce an output file")
                                      : QStringLiteral("timed out waiting for the recording result");
        }

        QString report_path;
        if (benchmark_mode) {
            const benchmark::RunConfig config = BuildBenchmarkRunConfig(options, frontend, cycle + 1, selected_target);

            // Read the terminal engine snapshot through the coordinator's read-back
            // accessor rather than taking its single diagnostics callback slot — the
            // frontend owns that slot, and displacing it is a known way to silently
            // zero every drop counter for the session.
            recorder_core::RecordingDiagnosticsSnapshot snapshot;
            if (!coordinator.LastDiagnosticsSnapshot(&snapshot))
                snapshot = recorder_core::RecordingDiagnosticsSnapshot{};

            // Read back what the engine was actually handed. Not derived from the
            // options above: a frontend can commit settings of its own between the
            // CLI parse and StartRecording, and that is precisely the defect that
            // invalidated an earlier Widgets-vs-Quick comparison.
            recorder_core::RecorderConfig committed;
            const benchmark::EffectiveRecordingConfig effective = coordinator.LastCommittedRecorderConfig(&committed)
                                                                      ? benchmark::DescribeEffectiveConfig(committed)
                                                                      : benchmark::UnavailableEffectiveConfig();

            const benchmark::PreviewMetrics preview =
                hooks.samplePreviewMetrics ? hooks.samplePreviewMetrics() : benchmark::PreviewMetrics{};
            const benchmark::RecordingMetrics recording = benchmark::RecordingMetricsFromSnapshot(
                snapshot, baseline_snapshot, measurement_started ? options.duration_seconds : 0.0);
            // The measured window is the requested duration, never the wall clock:
            // the warm-up is deliberately outside it.
            const benchmark::ProcessMetrics process =
                process_sampler.Sample(measurement_started ? options.duration_seconds : 0.0);

            report_path = QDir(options.benchmark_output_dir)
                              .filePath(benchmark::ArtifactBaseName(config) + QStringLiteral(".json"));
            const bool wrote = QDir().mkpath(options.benchmark_output_dir) &&
                               benchmark::WriteReport(report_path, environment, config, effective, outcome, preview,
                                                      recording, process);
            if (!wrote) {
                // A measured run whose numbers did not reach disk is a failed run,
                // not a successful recording with a missing side effect.
                ok = false;
                report_path.clear();
                if (final_error.isEmpty())
                    final_error =
                        QStringLiteral("benchmark report could not be written to %1").arg(options.benchmark_output_dir);
            }
        }

        PrintResultLine(ResultToJson(ok, final_output_path, report_path, final_error));
        all_ok = all_ok && ok;
        if (!ok)
            break; // don't keep cycling once one cycle has already failed/hung
    }
    return all_ok ? 0 : 1;
}

int RunAutoRecord(QCoreApplication& app, const AutoRecordOptions& options) {
    // Bare mode: own the coordinator directly, then run the shared drive loop on it.
    // No preview exists here, so the preview half of a benchmark report is honestly
    // unavailable while the recording half stays fully comparable.
    exosnap::RecordingCoordinator coordinator;
    return RunAutoRecordOnCoordinator(app, coordinator, options, benchmark::Frontend::Headless);
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)

namespace {

// Waits (bounded) for the Record page's coordinator to be built AND brought to Ready by
// the REAL async capability probe. Ready means the probe has already delivered
// (OnCapabilitiesReady ran on the UI thread), so a later queued delivery cannot clobber
// the Recording state mid-run once RunAutoRecordOnCoordinator enters its event loop.
//
// The wait is a bounded poll (25ms interval, up to timeout_ms) of real, in-memory
// getters — page.recordingCoordinator() for non-null, then coordinator->State() — not
// a connection to coordinatorInitialized() or any other signal, and not a log scrape.
// RecordPage owns the single state-changed callback and exposes no "became Ready"
// signal a non-owner can connect to, so polling the getters is the only observation
// point available. Returns the coordinator on Ready, or nullptr on a capability block
// or timeout (with a reason written to *error).
exosnap::RecordingCoordinator* WaitForCoordinatorReady(exosnap::RecordPage& page, int timeout_ms, QString* error) {
    QElapsedTimer clock;
    clock.start();
    QEventLoop loop;
    QTimer poll;
    exosnap::RecordingCoordinator* ready = nullptr;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        exosnap::RecordingCoordinator* coordinator = page.recordingCoordinator();
        if (coordinator != nullptr) {
            const UiRecordingState state = coordinator->State();
            if (state == UiRecordingState::Ready) {
                ready = coordinator;
                loop.quit();
                return;
            }
            if (state == UiRecordingState::Blocked) {
                if (error != nullptr)
                    *error = QString::fromStdWString(coordinator->CapabilityStatusText());
                loop.quit();
                return;
            }
        }
        if (clock.elapsed() >= timeout_ms) {
            if (error != nullptr)
                *error = QStringLiteral("coordinator did not reach Ready within %1 ms").arg(timeout_ms);
            loop.quit();
        }
    });
    poll.start(25);
    loop.exec();
    return ready;
}

} // namespace

int RunAutoRecord(QApplication& app, exosnap::MainWindow& window, const AutoRecordOptions& options) {
    if (!options.enable_preview) {
        // --enable-preview absent: fall back to headless bare mode, no window shown.
        return RunAutoRecord(app, options);
    }
    if (options.target == TargetKind::Region) {
        return FailWith(RegionNotSupportedError());
    }

    // Never activate (WA_ShowWithoutActivating) and prefer a non-primary screen, so
    // the developer's primary desktop is never covered — and, in the frontend A/B,
    // so the application is not inside the 1440p image it is capturing.
    //
    // The rule itself lives in benchmark::ResolveHarnessWindowPlacement: the Qt
    // Quick entry point places its window through the same call, which is what
    // makes "same logical size, equivalent placement" a fact rather than an
    // intention.
    window.setAttribute(Qt::WA_ShowWithoutActivating, true);
    const benchmark::HarnessWindowPlacement placement = benchmark::ResolveHarnessWindowPlacement();
    window.resize(placement.width, placement.height);
    window.showNormal();
    // showEvent restores persisted user geometry on first show; re-apply afterwards
    // so the window lands where the placement rule says, not where the last
    // interactive session left it.
    if (!placement.screen_name.isEmpty()) {
        window.move(placement.x, placement.y);
        window.resize(placement.width, placement.height);
    }

    exosnap::RecordPage* record_page = window.recordPage();
    if (record_page == nullptr)
        return FailWith(QStringLiteral("preview mode: MainWindow has no Record page"));

    // Let the real, worker-thread capability probe + deferred coordinator init run and
    // bring the coordinator to Ready through the live idle-preview path.
    QString wait_error;
    exosnap::RecordingCoordinator* coordinator = WaitForCoordinatorReady(*record_page, 20000, &wait_error);
    if (coordinator == nullptr)
        return FailWith(QStringLiteral("preview mode: %1").arg(wait_error));

    // Select the requested target through the same private path a source-picker click
    // uses, so the live preview shows exactly what will be recorded.
    VideoSettingsModel preview_video_settings = VideoSettingsModel::Defaults();
    preview_video_settings.frame_rate_num = static_cast<uint32_t>(options.frame_rate);
    preview_video_settings.frame_rate_den = 1;
    record_page->setVideoSettings(preview_video_settings);
    const auto want_kind = (options.target == TargetKind::Window) ? recorder_core::CaptureTarget::Kind::Window
                                                                  : recorder_core::CaptureTarget::Kind::Monitor;
    if (!record_page->selectCaptureTargetForAutomation(want_kind, options.target_window_title)) {
        const QString what = options.target == TargetKind::Monitor
                                 ? QStringLiteral("monitor")
                                 : QStringLiteral("window matching \"%1\"").arg(options.target_window_title);
        return FailWith(QStringLiteral("no matching capture target (%1)").arg(what));
    }

    if (options.capture_frame_in_ready) {
        // Exercises the DXGI-preview-renderer readback path specifically (Ready
        // state, idle preview, no recording) — the engine's own snapshot path
        // is already covered by --capture-frame-at during an active recording.
        // No recording is started; this is a standalone check.
        //
        // The DXGI preview renderer starts asynchronously on its own thread
        // (device/swap-chain/shader init, then the first present) after
        // selectCaptureTargetForAutomation returns, so the very first attempt
        // can race a preview that hasn't rendered its first frame yet — retry
        // on a short poll rather than treating that as a hard failure (a real
        // user clicking the button has been looking at an already-live preview
        // for seconds, so this race is a test-harness-only concern).
        bool have_result = false;
        bool succeeded = false;
        QString result_path;
        QString result_error;
        coordinator->SetFrameCapturedCallback([&](bool success, const QString& path, const QString& error) {
            have_result = true;
            succeeded = success;
            result_path = path;
            result_error = error;
        });

        QElapsedTimer clock;
        clock.start();
        QEventLoop loop;
        QTimer retryTimer;
        retryTimer.setInterval(200);
        QObject::connect(&retryTimer, &QTimer::timeout, &loop, [&]() {
            have_result = false;
            coordinator->CaptureFrame();
        });
        QTimer pollTimer;
        pollTimer.setInterval(25);
        QObject::connect(&pollTimer, &QTimer::timeout, &loop, [&]() {
            if ((have_result && succeeded) || clock.elapsed() >= 10000)
                loop.quit();
        });
        coordinator->CaptureFrame();
        retryTimer.start();
        pollTimer.start();
        loop.exec();
        retryTimer.stop();
        pollTimer.stop();

        if (!have_result) {
            return FailWith(QStringLiteral("capture-frame-in-ready: timed out waiting for the result"));
        }
        PrintResultLine(ResultToJson(succeeded, result_path, QString(), result_error));
        return succeeded ? 0 : 1;
    }

    // Drive the recording on the coordinator the Record page owns. RunAutoRecordOnCoordinator
    // re-delivers caps (a redundant Ready->Ready now that the async probe has already landed)
    // and, via RecordPage's still-wired state-changed callback, the idle preview is live
    // before the recording start.
    //
    // The two hooks below are the ONLY Widgets-specific code in a benchmark run.
    // Warm-up handling, timing, the engine metrics, the process sampling and the
    // report format all come from the shared drive loop, so the Qt Quick entry
    // point produces a document that differs only where the frontends genuinely do.
    BenchmarkHooks hooks;
    hooks.onMeasurementStart = [record_page]() { record_page->resetPreviewPerformanceMetrics(); };
    hooks.samplePreviewMetrics = [record_page, &window]() {
        const DxgiPreviewPerformanceSnapshot metrics = record_page->previewPerformanceMetrics();
        constexpr auto kSame = benchmark::Comparability::Identical;
        constexpr auto kApprox = benchmark::Comparability::Approximate;

        benchmark::PreviewMetrics preview;
        preview.frames_presented =
            benchmark::MakeMetric(static_cast<double>(metrics.presented_frames), kApprox,
                                  "DxgiPreviewRenderer: swap-chain Presents of the preview quad on its own "
                                  "DXGI render thread");
        preview.source_frames_consumed =
            benchmark::MakeMetric(static_cast<double>(metrics.pushed_frames_consumed), kSame,
                                  "DxgiPreviewRenderer: frames taken off the shared preview texture");
        preview.mutex_misses = benchmark::MakeMetric(
            static_cast<double>(metrics.keyed_mutex_misses), kApprox,
            "DxgiPreviewRenderer: keyed-mutex AcquireSync(0) failures, one per attempt — scales with the "
            "preview thread's own present cadence, not with the transport");
        preview.frame_cadence_fps =
            benchmark::MakeMetric(metrics.present_fps, kApprox, "DxgiPreviewRenderer: preview-quad present rate");
        preview.frame_ms_p50 =
            benchmark::MakeMetric(metrics.present_ms_p50, kApprox, "DxgiPreviewRenderer: inter-present interval");
        preview.frame_ms_p95 =
            benchmark::MakeMetric(metrics.present_ms_p95, kApprox, "DxgiPreviewRenderer: inter-present interval");
        preview.frame_ms_p99 =
            benchmark::MakeMetric(metrics.present_ms_p99, kApprox, "DxgiPreviewRenderer: inter-present interval");
        preview.frame_ms_max =
            benchmark::MakeMetric(metrics.present_ms_max, kApprox, "DxgiPreviewRenderer: inter-present interval");
        preview.source_delivery_fps =
            benchmark::MakeMetric(metrics.source_delivery_fps, kApprox,
                                  "DxgiPreviewRenderer: rate at which frames arrived AT THIS CONSUMER; bounded by "
                                  "the preview thread's own poll rate, not by what the engine produced");
        preview.source_interval_ms_p95 = benchmark::MakeMetric(
            metrics.source_interval_ms_p95, kApprox, "DxgiPreviewRenderer: consumer-observed arrival interval");
        preview.source_interval_ms_p99 = benchmark::MakeMetric(
            metrics.source_interval_ms_p99, kApprox, "DxgiPreviewRenderer: consumer-observed arrival interval");
        // The Widgets renderer keeps no p50 of the submit duration; reporting it as
        // unavailable is the honest answer, not interpolating from p95.
        preview.submit_us_p50 = benchmark::UnavailableMetric(kSame, "not sampled by DxgiPreviewRenderer");
        preview.submit_us_p95 =
            benchmark::MakeMetric(metrics.submit_us_p95, kSame, "DxgiPreviewRenderer: GPU submit for the preview copy");
        preview.submit_us_p99 =
            benchmark::MakeMetric(metrics.submit_us_p99, kSame, "DxgiPreviewRenderer: GPU submit for the preview copy");
        preview.child_hwnd_count = benchmark::MakeMetric(
            static_cast<double>(benchmark::CountChildWindows(reinterpret_cast<void*>(window.winId()))),
            benchmark::Comparability::FrontendOnly,
            "EnumChildWindows over the top-level HWND; the Widgets preview IS a native child window");
        if (metrics.pushed_frames_consumed > 0) {
            preview.render_amplification = benchmark::MakeMetric(
                static_cast<double>(metrics.presented_frames) / static_cast<double>(metrics.pushed_frames_consumed),
                kApprox, "DxgiPreviewRenderer: preview-quad Presents per frame taken off the shared texture");
        } else {
            preview.render_amplification = benchmark::UnavailableMetric(
                kApprox, "DxgiPreviewRenderer: no source frame was consumed in the window");
        }
        // The Widgets preview presents from a dedicated thread on a fixed
        // interval; there is no scheduling gate here to count.
        preview.preview_publish_signals = benchmark::UnavailableMetric(
            benchmark::Comparability::FrontendOnly, "no publish-edge scheduling in the Widgets preview");
        preview.preview_scene_update_requests = benchmark::UnavailableMetric(
            benchmark::Comparability::FrontendOnly, "no publish-edge scheduling in the Widgets preview");
        return preview;
    };

    const int rc = RunAutoRecordOnCoordinator(app, *coordinator, options, benchmark::Frontend::Widgets, hooks);

    if (!options.screenshot_path.isEmpty()) {
        if (!exosnap::visual::WriteVisualScreenshot(window, options.screenshot_path)) {
            diagnostics::AppLog::warning(
                QStringLiteral("auto-record"),
                QStringLiteral("preview screenshot write failed: %1").arg(options.screenshot_path));
        }
    }
    return rc;
}

#endif // EXOSNAP_ENABLE_VISUAL_TEST_HARNESS

} // namespace exosnap::auto_record
