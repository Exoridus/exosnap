#pragma once

// What must happen when the user tries to close the application window.
//
// This is product safety policy, not presentation: closing at the wrong moment
// corrupts a container being finalized, abandons an MP4 remux, or kills a
// running capture. The Widgets shell expressed it as a chain of early returns
// inside `MainWindow::closeEvent` with three `QMessageBox::exec()` calls. A QML
// dialog has no `exec()`, so the decision has to be a value the frontend can
// route asynchronously — which is what this header provides.
//
// Pure and frontend-free on purpose: the ordering below is the part that must
// not drift between the two frontends, and it is the part worth testing without
// a window.

#include <QString>

namespace exosnap {

// Everything the decision depends on, sampled at the moment of the close
// request. Deliberately booleans rather than a recording-state enum: the export
// is not a recording state at all, and mixing the two is what let the Widgets
// version grow four separate flag sources.
struct CloseGuardState {
    // Container finalize in flight (UiRecordingState::Stopping). Not
    // cancellable — aborting it corrupts the file being written.
    bool finalizing = false;
    // ADR-0014 MP4 remux after stop (UiRecordingState::Saving). Cancellable;
    // the transient MKV survives.
    bool remuxing = false;
    // Stream-copy export from the Edit surface. Cancellable and safe: an export
    // never mutates the original recording.
    bool exporting = false;
    // Capture still running (recording, paused, or counting down).
    bool recording = false;
};

enum class CloseGuardKind {
    // Nothing in flight — close immediately.
    Allow,
    // Refuse without a prompt. The finalizing overlay is already on screen for
    // the whole Stopping phase, so a dialog would only restate it, and there is
    // no safe way to proceed anyway.
    BlockSilently,
    ConfirmRemux,
    ConfirmExport,
    ConfirmRecording,
};

// What the frontend should show, if anything. Labels live here rather than in
// QML so the two frontends cannot describe the same consequence differently.
struct CloseGuardPrompt {
    CloseGuardKind kind = CloseGuardKind::Allow;
    QString title;
    QString body;
    // Label of the button that proceeds with closing (and applies the effect).
    QString proceed_label;
    // Label of the button that keeps the window open.
    QString cancel_label;
    // Which button is focused. Every guard except the recording one defaults to
    // the safe choice of waiting; the recording guard defaults to Cancel because
    // stopping a capture by accident is the more expensive mistake.
    bool default_is_cancel = true;
};

// Evaluates the guards in their authoritative order: finalize first (it cannot
// be waived at all), then remux, then export, then recording. The order is the
// policy — a close during both an export and a recording must ask about both,
// which the caller achieves by clearing the waived condition and evaluating
// again.
[[nodiscard]] CloseGuardPrompt EvaluateCloseGuard(const CloseGuardState& state);

} // namespace exosnap
