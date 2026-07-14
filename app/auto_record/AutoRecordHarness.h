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
    QString screenshot_path;           // preview mode only
};

bool HasAutoRecordRequest(const QStringList& args);
bool ParseAutoRecordOptions(const QStringList& args, AutoRecordOptions* out, QString* error);

// Headless "bare mode" drive loop: builds and drives a standalone
// exosnap::RecordingCoordinator directly from CLI-configured options, produces a
// real recording file, and prints exactly one JSON result line to stdout. No
// MainWindow, no preview window (preview mode is Task 3). Returns the process exit
// code: 0 on a successful recording, non-zero on any failure (target not found,
// StartRecording refused, capability block, timeout).
int RunAutoRecord(QApplication& app, const AutoRecordOptions& options);

// Shared drive loop: seeds the coordinator's capability gate, commits the CLI output
// format, selects the capture target, starts + stops the recording after the requested
// duration, and prints exactly one JSON result line. Both the bare-mode entry point
// (above) and the preview-mode entry point (below) call this on their coordinator —
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
