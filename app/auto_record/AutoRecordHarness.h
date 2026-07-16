#pragma once

#include <QString>
#include <QStringList>

class QApplication;

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
int RunAutoRecord(QApplication& app, const AutoRecordOptions& options);

// Shared drive loop: seeds the coordinator's capability gate, commits the CLI output
// format, selects the capture target, then runs options.repeat_cycles start/stop
// cycles on it (same coordinator instance across cycles, so a later cycle sees
// whatever "warm" capture-hub state the previous cycle left behind), printing one
// JSON result line per cycle. Both the bare-mode entry point (above) and the
// preview-mode entry point (below) call this on their coordinator —
// bare mode on a coordinator it constructs itself, preview mode on the one the Record
// page owns. Returns the process exit code.
int RunAutoRecordOnCoordinator(QApplication& app, RecordingCoordinator& coordinator, const AutoRecordOptions& options);

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
