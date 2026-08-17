#pragma once

// LiveVerifyAutomationState.h -- the product state the control channel is
// allowed to observe, as one flat value type.
//
// This is the public surface of `ui.getState`, and it is deliberately a
// PRODUCT vocabulary rather than a frontend one. There are no QObject pointers,
// no QML ids, no loader addresses and no pixel coordinates in it, and there must
// never be: a protocol field named after an implementation detail turns that
// detail into a compatibility promise, and the promise is then kept by never
// refactoring.
//
// Being a plain value with an equality operator is what makes `stateRevision`
// honest. The revision advances when this struct changes and at no other time,
// so the one thing a runner is told about it -- "a different revision means the
// observable state is different" -- is true by construction rather than by the
// discipline of whoever adds the next signal connection.

#include <QString>

#include <cstdint>

namespace exosnap::live_verify {

// Named page values, not the shell's integer index. `app.snapshot` has always
// answered a bare number, which means the protocol silently depends on the
// enumerator order of a C++ enum nobody thought of as public.
namespace page_name {
inline constexpr const char* kRecord = "record";
inline constexpr const char* kSettings = "settings";
inline constexpr const char* kDiagnostics = "diagnostics";
inline constexpr const char* kLogs = "logs";
inline constexpr const char* kAbout = "about";
} // namespace page_name

// The three full-window modal surfaces, by product name. Empty means none is
// up, which serializes as JSON null.
namespace blocking_surface_name {
inline constexpr const char* kRecovery = "recovery";
inline constexpr const char* kCrashReport = "crashReport";
inline constexpr const char* kRecordingError = "recordingError";
} // namespace blocking_surface_name

struct AutomationState {
    // --- Where the user is ---------------------------------------------------
    QString page = QString::fromLatin1(page_name::kRecord);
    // Which of the three blocking surfaces is on screen, or empty for none.
    // Sourced from BlockingSurfaceArbiter itself -- the same object that decides
    // which one may be up -- so this can never disagree with what is composited.
    QString blocking_surface;

    // --- Record --------------------------------------------------------------
    // The UiRecordingState enumerator's name, not its number.
    QString recording_state;
    QString selected_source_name;
    QString selected_source_kind;
    bool source_picker_open = false;
    bool can_start = false;
    bool can_stop = false;
    bool can_pause = false;
    bool can_resume = false;
    bool can_split = false;
    bool can_capture_frame = false;
    bool can_select_source = false;

    // --- Edit ----------------------------------------------------------------
    // QCR-001: an open session is state of the Record destination, so "open" and
    // "on screen" are two facts and both are reported. `edit_visible` is false
    // while the user is on another page with the session still loaded.
    bool edit_session_open = false;
    bool edit_visible = false;
    bool edit_export_running = false;
    // "playing" | "paused" | "none" (no clip loaded).
    QString edit_playback = QStringLiteral("none");
    bool can_open_edit = false;

    // --- Shell surfaces ------------------------------------------------------
    bool notification_hub_open = false;

    // --- Update --------------------------------------------------------------
    // The update card's own state string, verbatim: "unchecked" | "checking" |
    // "uptodate" | "available" | "scoop" | "updater-running" | "pending" |
    // "verify-reinstall" | "error". Deliberately the SAME vocabulary the product
    // uses rather than a parallel enum -- a second spelling of the same fact is
    // a second thing that can be wrong, and this one is what the user sees.
    QString update_state;
    QString update_channel;
    // The running build's full version, and the offered release tag verbatim
    // (empty when nothing is offered). The second is the string that becomes the
    // updater's pinned --target-version, which is what makes "installed what was
    // offered" assertable across the process boundary.
    QString update_current_version;
    QString update_available_version;
    bool update_checking = false;
    bool update_available = false;
    // Whether the card's primary action is live right now. The precondition for
    // update.apply reads THIS rather than re-deriving it, so the button and the
    // command cannot disagree.
    bool update_action_enabled = false;
    // Why an update action is refused, in product vocabulary: "" (nothing in the
    // way) | "recording" | "finalizing" | "scoop" | "updaterRunning" |
    // "restartPending". Not a message -- a runner branches on this.
    QString update_blocker;

    [[nodiscard]] bool blocked() const noexcept {
        return !blocking_surface.isEmpty();
    }

    [[nodiscard]] bool operator==(const AutomationState&) const = default;
};

} // namespace exosnap::live_verify
