#pragma once

#include "../benchmark/BenchmarkReport.h"

#include <QString>
#include <QStringList>

#include <functional>

class QCoreApplication;

namespace exosnap {
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
    // Canonical CQ for the run, 1-51, defaulting to the Balanced tier. Spelled as
    // plain integers because this struct is parsed before anything else exists
    // and its test target links Qt only; AutoRecordHarness.cpp static_asserts
    // them against recorder_core's canonical values, so the duplication cannot
    // drift silently.
    static constexpr int kCqMin = 1;
    static constexpr int kCqMax = 51;
    static constexpr int kCqDefault = 24;
    int cq = kCqDefault;
    // NVENC speed/quality preset, 1-7 (P1..P7), defaulting to the shipped P4.
    // Same plain-integer treatment and the same static_assert pairing as the CQ
    // range above. Under constant QP this is an encode-time control, which is
    // exactly why a headroom run needs to vary it.
    static constexpr int kNvencPresetMin = 1;
    static constexpr int kNvencPresetMax = 7;
    static constexpr int kNvencPresetDefault = 4;
    int nvenc_preset = kNvencPresetDefault;
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
// (options.repeat_cycles, default 1). No window of any kind. Returns the process
// exit code: 0 when every cycle succeeded,
// non-zero on any failure (target not found, StartRecording refused, capability
// block, timeout) — cycling stops at the first failed cycle.
int RunAutoRecord(QCoreApplication& app, const AutoRecordOptions& options);

// Shared drive loop: seeds the coordinator's capability gate, commits the CLI output
// format, selects the capture target, then runs options.repeat_cycles start/stop
// cycles on it (same coordinator instance across cycles, so a later cycle sees
// whatever "warm" capture-hub state the previous cycle left behind), printing one
// JSON result line per cycle. Bare mode calls this on a coordinator it constructs
// itself; the application entry point calls it on the one the shell owns. Returns
// the process exit code.
//
// It was the single orchestration path for the frontend A/B benchmark, which is
// why configuration, timing and reporting all live here rather than in a caller:
// both frontends had to commit identical settings, select the target by an
// identical rule and run an identical warm-up/measure/stop sequence for the
// numbers to mean anything. Only one frontend is left, and the structure is kept
// because it is also what makes a run reproducible.
//
// `out_last_outcome`, when given, receives the last cycle's result: output path,
// media duration, dimensions. It exists because this loop takes the
// coordinator's single SetResultReadyCallback slot for itself, which displaces
// the frontend's own handler — so after a harness run the application's view
// model does not know a recording completed, and anything downstream (the
// Record -> Editor handoff) has nothing to act on. Rather than have the caller
// reconstruct that from the printed JSON line, hand it back directly.
int RunAutoRecordOnCoordinator(QCoreApplication& app, RecordingCoordinator& coordinator,
                               const AutoRecordOptions& options, benchmark::Frontend frontend,
                               const BenchmarkHooks& hooks = {}, benchmark::RunOutcome* out_last_outcome = nullptr);

} // namespace exosnap::auto_record
