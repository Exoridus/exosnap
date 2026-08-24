#pragma once

// What the shell surfaces show, and what they are allowed to offer.
//
// WHY THIS EXISTS
// ---------------
// The tray menu, the taskbar thumbnail buttons and the in-app transport all
// answer the same three questions -- is a recording running, may it be paused,
// may it be stopped -- and every surface that answered them for itself has
// eventually disagreed with the others. This is one projection of the recording
// state, read by all of them.
//
// It is NOT a second state machine. It decides nothing: `UiRecordingState` and
// the RecordViewModel's Can* predicates remain the authority on what is legal,
// and this file only translates their answer into shell affordances. Nothing
// here starts, pauses or stops anything.

#include <QMetaType>
#include <QtGlobal>

namespace exosnap {

enum class UiRecordingState;

// The shell's view of the session. Coarser than UiRecordingState on purpose:
// the shell distinguishes states it renders differently, and the recording
// engine's own distinctions (Countdown vs Preparing, Stopping vs Saving) are not
// among them.
enum class ShellPhase {
    // Nothing is running and a recording can be started.
    Idle,
    // Nothing is running and a recording cannot be started -- capabilities are
    // still loading, a region is being drawn, or an admission check refused.
    Blocked,
    // Committed to a capture that has not produced frames yet (countdown, device
    // setup). From the user's point of view the recording has begun.
    Preparing,
    Recording,
    Paused,
    // Stopping or remuxing. Terminal, but not finished.
    Finalizing,
    // A recording that finished successfully, for a bounded dwell. Time-boxed
    // because an icon that stays green has stopped describing the application
    // and started describing history.
    Saved,
};

// What the tray icon and the taskbar overlay badge SAY. Fewer values than the
// phase: the transitional phases have no badge of their own, and read as the
// state they are on their way into or out of.
enum class ShellIconState {
    Idle,
    Recording,
    Paused,
    Saved,
};

// A product intent a shell surface can ask for. `None` is the answer to a click
// that must do nothing -- an unknown button, or one whose action the current
// state does not allow.
enum class ShellAction {
    None,
    Start,
    Pause,
    Resume,
    Stop,
};

// The three registered taskbar thumbnail buttons. Pause and Resume share one
// slot: the set is fixed after ThumbBarAddButtons, so spending two registrations
// on mutually exclusive actions wastes one.
enum class ShellButton {
    Record,
    PauseResume,
    Stop,
};

// Native command ids for the thumbnail buttons, carried in the low word of a
// WM_COMMAND wParam. Values are arbitrary; what matters is that they are stable
// and do not collide with anything else this window's WM_COMMAND carries (the
// borderless shell registers no menus and no accelerators, so nothing else does).
inline constexpr int kShellButtonIdRecord = 0x7A01;
inline constexpr int kShellButtonIdPauseResume = 0x7A02;
inline constexpr int kShellButtonIdStop = 0x7A03;

// Everything the projection reads. Assembled by the caller from the recording
// view model, so the Can* predicates arrive as their owner computed them rather
// than re-derived here.
struct ShellPresenceInput {
    UiRecordingState state{};
    bool can_start = false;
    bool can_stop = false;
    bool can_pause = false;
    bool can_resume = false;
    // Whether the bounded post-recording dwell is still running. Honoured only
    // in a state that actually finished a recording: a dwell that outlives its
    // recording must not be able to paint a live session green, and refusing it
    // here means the guard holds even if the timer that owns the flag is wrong.
    bool saved_dwell_active = false;
};

// The projection every shell surface reads.
struct ShellPresenceState {
    ShellPhase phase = ShellPhase::Idle;
    ShellIconState icon_state = ShellIconState::Idle;

    bool can_start = false;
    bool can_pause = false;
    bool can_resume = false;
    bool can_stop = false;

    bool recording = false;
    bool paused = false;
    // A transition the user cannot interrupt: preparing a capture, or finalizing
    // one. What makes the transport read as working rather than as broken.
    bool busy = false;
    bool saved = false;
};

[[nodiscard]] bool operator==(const ShellPresenceState& lhs, const ShellPresenceState& rhs) noexcept;
[[nodiscard]] bool operator!=(const ShellPresenceState& lhs, const ShellPresenceState& rhs) noexcept;

[[nodiscard]] ShellPresenceState ProjectShellPresence(const ShellPresenceInput& input) noexcept;

// How one thumbnail button presents itself, and what a click on it would mean.
//
// A button that is not valid in a state is HIDDEN when its absence is the
// honest description (there is nothing to pause) and DISABLED when the action
// exists but is momentarily refused (a result surface still holding the
// session). A control that vanishes reads as a bug; a greyed one reads as a
// reason.
struct ShellButtonAppearance {
    bool visible = false;
    bool enabled = false;
    ShellAction action = ShellAction::None;
};

[[nodiscard]] ShellButtonAppearance ShellButtonFor(ShellButton button, const ShellPresenceState& state) noexcept;

// Maps a WM_COMMAND id to one of our buttons. False for anything else -- the
// filter is process-wide and the window is not the only source of commands.
[[nodiscard]] bool ShellButtonFromCommandId(int command_id, ShellButton& out) noexcept;

// The product intent a click on `command_id` carries in `state`, gated by that
// state's own appearance table. Returns None when the click must do nothing,
// which is the whole point: a stale thumbnail strip cannot ask for an action the
// state does not allow, because the same table draws it and answers it.
[[nodiscard]] ShellAction ResolveShellCommand(int command_id, const ShellPresenceState& state) noexcept;

} // namespace exosnap

// Declared so a queued connection and a QSignalSpy can carry the action across
// the shell boundary: the taskbar's button click arrives on the message pump and
// is delivered as a signal parameter.
Q_DECLARE_METATYPE(exosnap::ShellAction)
Q_DECLARE_METATYPE(exosnap::ShellIconState)
Q_DECLARE_METATYPE(exosnap::ShellPhase)
