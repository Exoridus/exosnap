#pragma once

#include "../benchmark/BenchmarkReport.h"

#include <QString>
#include <QStringList>

#include <functional>

class QApplication;
class QCoreApplication;

namespace exosnap {
class MainWindow;
class RecordingCoordinator;
} // namespace exosnap

namespace exosnap::auto_record {

enum class TargetKind { Monitor, Window, Region };
enum class HdrMode { Off, Tonemap, Native };

struct AutoRecordOptions {
    bool enable_preview = false;
    TargetKind target = TargetKind::Monitor;
    QString target_window_title;                  // required when target == Window
    QStringList audio_rows;                       // subset of {"app","sys","mic"}, order = row order
    QString merge_above;                          // row name that merges into the row above, or empty
    QString container = QStringLiteral("mkv");    // "mkv" | "mp4" | "webm"
    QString video_codec = QStringLiteral("av1");  // "h264" | "hevc" | "av1"
    QString audio_codec = QStringLiteral("opus"); // "opus" | "aac" | "pcm"
    int chroma = 420;                             // 420 | 444
    int bit_depth = 8;                            // 8 | 10
    HdrMode hdr_mode = HdrMode::Off;
    int frame_rate = 60; // 1-240; the product's own Expert range
    int duration_seconds = 10;
    int capture_frame_at_seconds = -1; // -1 = disabled
    // Preview mode only: capture a frame while the coordinator is still Ready
    // (idle preview, before recording starts) instead of running a recording at
    // all. Exercises the DXGI-preview-renderer readback path specifically (the
    // engine's own snapshot path is already covered by capture_frame_at_seconds
    // during an active recording). Reports one JSON result line and exits —
    // no recording is started when this is set.
    bool capture_frame_in_ready = false;
    QString screenshot_path; // preview mode only
    int repeat_cycles = 1;   // run N start/stop cycles on the same coordinator
                             // (warm capture-hub state) instead of exiting after one

    // Pause/resume inside the recording. -1 disables. The pause happens
    // pause_at_seconds into the run and lasts pause_for_seconds, which is added
    // to the wall-clock budget rather than taken out of duration_seconds: the
    // recorded MEDIA length stays what was asked for, which is what makes a
    // paused run comparable with an unpaused one and what lets the exported
    // duration be checked against an expectation.
    int pause_at_seconds = -1;
    int pause_for_seconds = 2;

    // ---- Frontend A/B benchmark mode -------------------------------------
    // Non-empty benchmark_scenario turns the ordinary drive loop into a measured
    // run: the recording is extended by benchmark_warmup_seconds, the preview and
    // process counters are reset when the warm-up ends, and one report per cycle is
    // written under benchmark_output_dir.
    //
    // There is deliberately no --benchmark-frontend flag. The frontend is a property
    // of the executable being driven, and each entry point states its own; a flag
    // could disagree with the binary that wrote the file.
    QString benchmark_scenario;
    QString benchmark_output_dir;
    int benchmark_warmup_seconds = 0;
    // Operator-supplied facts no in-process probe can know: the external source
    // being captured, its graphics preset, and whether frame generation was on.
    QString benchmark_source_notes;
};

// Frontend-specific probes the shared drive loop calls at the two moments that
// matter. Everything else about a benchmark run — configuration, timing, the
// engine metrics, the report — is common code, which is what makes the two
// frontends' numbers comparable at all.
struct BenchmarkHooks {
    // Warm-up has ended and the measured window opens. Called on the Qt main
    // thread while the recording is already active; the frontend resets its
    // preview counters so no start-up transient lands in the measurement.
    std::function<void()> onMeasurementStart;

    // The recording result has landed. Called on the Qt main thread; the frontend
    // reads its own preview instrumentation into the neutral contract.
    std::function<benchmark::PreviewMetrics()> samplePreviewMetrics;
};

bool HasAutoRecordRequest(const QStringList& args);
bool ParseAutoRecordOptions(const QStringList& args, AutoRecordOptions* out, QString* error);

// Headless "bare mode" drive loop: builds and drives a standalone
// exosnap::RecordingCoordinator directly from CLI-configured options, produces a
// real recording file, and prints one JSON result line to stdout per cycle
// (options.repeat_cycles, default 1). No MainWindow, no preview window (preview
// mode is Task 3). Returns the process exit code: 0 when every cycle succeeded,
// non-zero on any failure (target not found, StartRecording refused, capability
// block, timeout) — cycling stops at the first failed cycle.
int RunAutoRecord(QCoreApplication& app, const AutoRecordOptions& options);

// Shared drive loop: seeds the coordinator's capability gate, commits the CLI output
// format, selects the capture target, then runs options.repeat_cycles start/stop
// cycles on it (same coordinator instance across cycles, so a later cycle sees
// whatever "warm" capture-hub state the previous cycle left behind), printing one
// JSON result line per cycle. Bare mode calls this on a coordinator it constructs
// itself; the Widgets and Qt Quick preview entry points call it on the one their
// shell owns. Returns the process exit code.
//
// This is the single orchestration path for the frontend A/B benchmark. Both
// frontends therefore commit the identical output/video settings, select the target
// by the identical rule, and run the identical warm-up/measure/stop sequence — the
// only per-frontend code is the two BenchmarkHooks probes.
//
// Takes QCoreApplication& rather than QApplication& because it only ever quits and
// re-enters the event loop: the Widgets shell supplies a QApplication and the Quick
// shell a QGuiApplication, and neither distinction is meaningful here.
int RunAutoRecordOnCoordinator(QCoreApplication& app, RecordingCoordinator& coordinator,
                               const AutoRecordOptions& options, benchmark::Frontend frontend,
                               const BenchmarkHooks& hooks = {});

// Preview-mode drive loop: shows an OFF-SCREEN MainWindow (never activated, placed on a
// non-primary screen when one exists), waits for the real async capability probe to
// bring the Record page's coordinator up through the same idle-preview machinery the
// live app uses (NOT the frozen --visual-test fixture), records via
// RunAutoRecordOnCoordinator on that coordinator, and — when options.screenshot_path is
// set — writes a screenshot of the rendered Record page. Falls back to bare mode when
// options.enable_preview is false. Defined only in the visual-test-harness-enabled build
// (debug); declared here so main.cpp can call it.
int RunAutoRecord(QApplication& app, MainWindow& window, const AutoRecordOptions& options);

} // namespace exosnap::auto_record
