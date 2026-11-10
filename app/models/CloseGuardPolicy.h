#pragma once

// Product policy for asynchronous close decisions. Finalization cannot be
// waived; other operations and authored editor work require explicit consent.

#include <QString>

namespace exosnap {

// Everything the decision depends on, sampled at the moment of the close
// request. Operation generations identify the work described by a prompt.
struct CloseGuardState {
    // Container finalize in flight (UiRecordingState::Stopping). Not
    // cancellable — aborting it corrupts the file being written.
    bool finalizing = false;
    // MP4 remux after stop (UiRecordingState::Saving). Cancellable;
    // the transient MKV survives.
    bool remuxing = false;
    // Stream-copy export from the Edit surface. Cancellable and safe: an export
    // never mutates the original recording.
    bool exporting = false;
    // Capture still running (recording, paused, or counting down).
    bool recording = false;
    bool editor_dirty = false;
    // Identity tokens prevent a prompt or waiver from consenting to later work.
    quint64 remux_generation = 0;
    quint64 export_generation = 0;
    quint64 recording_generation = 0;
    quint64 editor_revision = 0;
};

enum class CloseGuardKind {
    // No operation or authored editor work needs confirmation.
    Allow,
    // Refuse without a prompt. The finalizing overlay is already on screen for
    // the whole Stopping phase, so a dialog would only restate it, and there is
    // no safe way to proceed anyway.
    BlockSilently,
    ConfirmRemux,
    ConfirmExport,
    ConfirmRecording,
    ConfirmEditor,
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
    // Every prompt focuses the safe choice that keeps the application open.
    bool default_is_cancel = true;
};

// Evaluates the guards in their authoritative order: finalize first (it cannot
// be waived at all), then remux, export, recording and editor changes. A close
// during both an export and a recording must ask about both,
// which the caller achieves by clearing the waived condition and evaluating
// again.
[[nodiscard]] CloseGuardPrompt EvaluateCloseGuard(const CloseGuardState& state);

} // namespace exosnap
