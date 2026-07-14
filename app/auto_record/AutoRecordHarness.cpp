#include "AutoRecordHarness.h"

#include <filesystem>

#include <QApplication>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

#include <capability/audio_ui_state.h>
#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <capability/resolver.h>
#include <capability/user_config.h>
#include <recorder_core/audio_track_model.h>
#include <recorder_core/codec_types.h>
#include <recorder_core/recorder_session.h>

#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../services/RecordingCoordinator.h"
#include "../viewmodels/RecordViewModel.h"

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QScreen>

#include "../MainWindow.h"
#include "../diagnostics/AppLog.h"
#include "../pages/RecordPage.h"
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
// These mirror the field-for-field translation RecordPage::primaryRecorderConfig()
// and the Output settings model do from live UI state; here the "UI state" is the
// parsed AutoRecordOptions instead. The CLI parser (above) already restricts the
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
        return capability::VideoCodec::H264Nvenc;
    if (name == QStringLiteral("hevc"))
        return capability::VideoCodec::HevcNvenc;
    return capability::VideoCodec::Av1Nvenc; // "av1"
}

capability::AudioCodec MapAudioCodec(const QString& name) {
    if (name == QStringLiteral("aac"))
        return capability::AudioCodec::AacMf;
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

// Builds the capability::UserRecorderConfig that seeds the resolver validation and
// the coordinator's Ready gate (OnCapabilitiesReady). This is the harness's stand-in
// for RecordPage::primaryRecorderConfig(), which reads the same fields off live
// Settings state. The actual format the recording uses is committed separately via
// SetOutputSettings (see BuildOutputSettings), which re-reconciles container×codec.
capability::UserRecorderConfig BuildUserRecorderConfig(const AutoRecordOptions& options) {
    capability::UserRecorderConfig config;
    config.container = MapContainer(options.container);
    config.video_codec = MapVideoCodec(options.video_codec);
    config.audio_codec = MapAudioCodec(options.audio_codec);
    config.chroma = MapChroma(options.chroma);
    config.bit_depth = MapBitDepth(options.bit_depth);
    config.hdr_mode = MapHdrMode(options.hdr_mode);
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;
    return config;
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

} // namespace

int RunAutoRecordOnCoordinator(QApplication& app, exosnap::RecordingCoordinator& coordinator,
                               const AutoRecordOptions& options) {
    if (options.target == TargetKind::Region) {
        // Region capture (Monitor + crop_region) is out of the v1 checklist scope for
        // the harness; reject it honestly rather than silently recording the full monitor.
        return FailWith(RegionNotSupportedError());
    }

    // Synchronous capability probe. Bare mode has no UI responsiveness constraint, so
    // it skips the worker-thread hop MainWindow uses (app/MainWindow.cpp:1093-1109).
    const capability::CapabilitySet caps = capability::CapabilityBuilder::BuildFromHardwareQuery();

    // Seed the coordinator's Ready gate. Mirrors RecordPage::deliverCapabilitiesToCoordinator():
    // resolve the requested format against the probed caps, then hand both to the coordinator.
    const capability::UserRecorderConfig user_config = BuildUserRecorderConfig(options);
    const capability::SettingsResolver resolver(caps);
    const capability::ResolveResult validation = resolver.ValidateConfig(user_config);
    coordinator.OnCapabilitiesReady(caps, validation);

    if (!validation.succeeded || coordinator.State() != UiRecordingState::Ready) {
        const QString reason = validation.invalidity.empty()
                                   ? QStringLiteral("recording unavailable for the requested format")
                                   : QString::fromStdString(validation.invalidity.front().message);
        return FailWith(QStringLiteral("recording unavailable: %1").arg(reason));
    }

    // Commit the actual output format (container/codec/depth/chroma/HDR) and frame rate.
    // SetOutputSettings re-reconciles the format and stamps it into the resolved config
    // the recording thread reads, so this — not the resolver seed above — decides the
    // container/codec the file is written with.
    coordinator.SetOutputSettings(BuildOutputSettings(options));
    coordinator.SetVideoSettings(VideoSettingsModel::Defaults());

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

    // Result is delivered via callback, posted to this (Qt main) thread by the
    // coordinator — possibly asynchronously after a background remux for MP4. Capture
    // it and quit the loop as soon as it lands; the safety timer below is only a
    // fallback for a hang.
    QString final_output_path;
    QString final_error;
    bool have_result = false;
    bool succeeded = false;
    coordinator.SetResultReadyCallback([&](const UiRecordingResult& result) {
        have_result = true;
        succeeded = result.succeeded;
        final_output_path = QString::fromStdWString(result.output_path);
        final_error = QString::fromStdWString(result.error_detail);
        app.quit();
    });

    const capability::AudioUiState audio_state = BuildAudioUiState(options);
    if (!coordinator.StartRecording(selected_target, audio_state)) {
        return FailWith(QStringLiteral("StartRecording refused (coordinator not ready or busy)"));
    }

    // Optional mid-recording frame capture (PNG to the output folder).
    if (options.capture_frame_at_seconds > 0) {
        QTimer::singleShot(options.capture_frame_at_seconds * 1000, &app,
                           [&coordinator]() { coordinator.CaptureFrame(); });
    }

    // Stop after the requested duration.
    QTimer::singleShot(options.duration_seconds * 1000, &app, [&coordinator]() { coordinator.StopRecording(); });

    // Safety net: if the result never arrives (hang, crash-in-teardown), quit anyway so
    // the process exits with a failure rather than blocking forever. Generous enough to
    // cover finalize + an MP4 remux of a short clip.
    constexpr int kGraceMs = 30000;
    QTimer::singleShot(options.duration_seconds * 1000 + kGraceMs, &app, [&app]() { app.quit(); });

    app.exec();

    const bool ok = have_result && succeeded && final_error.isEmpty() && !final_output_path.isEmpty();
    if (!ok && final_error.isEmpty()) {
        final_error = have_result ? QStringLiteral("recording did not produce an output file")
                                  : QStringLiteral("timed out waiting for the recording result");
    }
    PrintResultLine(ResultToJson(ok, final_output_path, QString(), final_error));
    return ok ? 0 : 1;
}

int RunAutoRecord(QApplication& app, const AutoRecordOptions& options) {
    // Bare mode: own the coordinator directly, then run the shared drive loop on it.
    exosnap::RecordingCoordinator coordinator;
    return RunAutoRecordOnCoordinator(app, coordinator, options);
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

    // Off-screen placement — mirrors VisualTestHarness capture-mode geometry: never
    // activate (WA_ShowWithoutActivating) and prefer a non-primary screen so the
    // developer's primary desktop is never covered.
    window.setAttribute(Qt::WA_ShowWithoutActivating, true);
    QScreen* target_screen = nullptr;
    {
        const QList<QScreen*> screens = QGuiApplication::screens();
        QScreen* const primary = QGuiApplication::primaryScreen();
        for (QScreen* screen : screens) {
            if (screen != primary) {
                target_screen = screen;
                break;
            }
        }
        if (target_screen == nullptr)
            target_screen = primary;
    }
    window.resize(1280, 820);
    window.showNormal();
    // showEvent restores persisted user geometry on first show; re-apply the off-screen
    // placement afterwards so the window lands on the intended (non-primary) screen.
    if (target_screen != nullptr) {
        window.move(target_screen->availableGeometry().topLeft());
        window.resize(1280, 820);
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
    const auto want_kind = (options.target == TargetKind::Window) ? recorder_core::CaptureTarget::Kind::Window
                                                                  : recorder_core::CaptureTarget::Kind::Monitor;
    if (!record_page->selectCaptureTargetForAutomation(want_kind, options.target_window_title)) {
        const QString what = options.target == TargetKind::Monitor
                                 ? QStringLiteral("monitor")
                                 : QStringLiteral("window matching \"%1\"").arg(options.target_window_title);
        return FailWith(QStringLiteral("no matching capture target (%1)").arg(what));
    }

    // Drive the recording on the coordinator the Record page owns. RunAutoRecordOnCoordinator
    // re-delivers caps (a redundant Ready->Ready now that the async probe has already landed)
    // and, via RecordPage's still-wired state-changed callback, the idle preview is live
    // before the recording start.
    const int rc = RunAutoRecordOnCoordinator(app, *coordinator, options);

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
