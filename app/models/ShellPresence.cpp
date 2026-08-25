#include "models/ShellPresence.h"

#include "viewmodels/RecordViewModel.h"

namespace exosnap {

namespace {

// The phase, ignoring the Saved dwell entirely. Split out so the dwell can only
// ever be applied on top of a phase that already describes a finished recording:
// a flag that outlived its recording then has nothing to attach to.
[[nodiscard]] ShellPhase BasePhase(UiRecordingState state) noexcept {
    switch (state) {
    case UiRecordingState::Ready:
    case UiRecordingState::Completed:
        return ShellPhase::Idle;
    case UiRecordingState::Failed:
        return ShellPhase::Failed;
    case UiRecordingState::LoadingCapabilities:
    case UiRecordingState::Blocked:
    case UiRecordingState::RegionSelecting:
        return ShellPhase::Blocked;
    case UiRecordingState::Countdown:
    case UiRecordingState::Preparing:
        return ShellPhase::Preparing;
    case UiRecordingState::Recording:
        return ShellPhase::Recording;
    case UiRecordingState::Paused:
    case UiRecordingState::ArmedFromRecovery:
        return ShellPhase::Paused;
    case UiRecordingState::Stopping:
    case UiRecordingState::Saving:
        return ShellPhase::Finalizing;
    }
    // No `default:` above, so a new UiRecordingState fails the build rather than
    // silently presenting itself to the user as idle. This is only the
    // unreachable tail the compiler still wants.
    return ShellPhase::Blocked;
}

[[nodiscard]] ShellIconState IconStateFor(ShellPhase phase) noexcept {
    switch (phase) {
    case ShellPhase::Recording:
    // Preparing already reads as Recording, and always has: the capture is
    // committed from the user's point of view, so a badge that waits for the
    // first frame describes the encoder rather than the session.
    case ShellPhase::Preparing:
        return ShellIconState::Recording;
    case ShellPhase::Paused:
        return ShellIconState::Paused;
    case ShellPhase::Saved:
        return ShellIconState::Saved;
    // Neither coral nor green is true while the file is being written, and the
    // processing mark is the one that says so. What keeps it from flashing on
    // every stop is the settle in quick::ShellPresenceAdapter, not this table.
    case ShellPhase::Finalizing:
        return ShellIconState::Processing;
    case ShellPhase::Failed:
        return ShellIconState::Error;
    case ShellPhase::Idle:
    case ShellPhase::Blocked:
        return ShellIconState::Idle;
    }
    return ShellIconState::Idle;
}

} // namespace

bool operator==(const ShellPresenceState& lhs, const ShellPresenceState& rhs) noexcept {
    return lhs.phase == rhs.phase && lhs.icon_state == rhs.icon_state && lhs.can_start == rhs.can_start &&
           lhs.can_pause == rhs.can_pause && lhs.can_resume == rhs.can_resume && lhs.can_stop == rhs.can_stop &&
           lhs.recording == rhs.recording && lhs.paused == rhs.paused && lhs.busy == rhs.busy &&
           lhs.saved == rhs.saved && lhs.failed == rhs.failed;
}

bool operator!=(const ShellPresenceState& lhs, const ShellPresenceState& rhs) noexcept {
    return !(lhs == rhs);
}

ShellPresenceState ProjectShellPresence(const ShellPresenceInput& input) noexcept {
    ShellPresenceState state;

    state.phase = BasePhase(input.state);
    // The dwell is honoured only where a recording actually finished well.
    // Completed is the single state that says so; Failed reaching this branch
    // would paint a failure green.
    if (state.phase == ShellPhase::Idle && input.saved_dwell_active && input.state == UiRecordingState::Completed)
        state.phase = ShellPhase::Saved;

    state.icon_state = IconStateFor(state.phase);

    state.can_start = input.can_start;
    state.can_pause = input.can_pause;
    state.can_resume = input.can_resume;
    state.can_stop = input.can_stop;

    state.recording = state.phase == ShellPhase::Recording;
    state.paused = state.phase == ShellPhase::Paused;
    state.busy = state.phase == ShellPhase::Preparing || state.phase == ShellPhase::Finalizing;
    state.failed = state.phase == ShellPhase::Failed;
    state.saved = state.phase == ShellPhase::Saved;

    return state;
}

ShellButtonAppearance ShellButtonFor(ShellButton button, const ShellPresenceState& state) noexcept {
    ShellButtonAppearance appearance;

    // `action` is what the button MEANS, and stays set while it is greyed: the
    // icon and the tooltip are drawn from it, and a disabled Pause that reports
    // no meaning has nothing to label itself with. Whether the meaning may be
    // acted on is `enabled`, and ResolveShellCommand is the one place that asks.
    switch (button) {
    case ShellButton::Record:
        // Present in every state that is not live, so the strip never changes
        // width; enabled only where the session's own policy allows a start.
        appearance.visible = !state.recording && !state.paused;
        appearance.enabled = appearance.visible && state.can_start;
        appearance.action = appearance.visible ? ShellAction::Start : ShellAction::None;
        return appearance;

    case ShellButton::PauseResume:
        appearance.visible = state.recording || state.paused;
        if (!appearance.visible)
            return appearance;
        if (state.recording) {
            appearance.enabled = state.can_pause;
            appearance.action = ShellAction::Pause;
        } else {
            appearance.enabled = state.can_resume;
            appearance.action = ShellAction::Resume;
        }
        return appearance;

    case ShellButton::Stop:
        appearance.visible = state.recording || state.paused;
        appearance.enabled = appearance.visible && state.can_stop;
        appearance.action = appearance.visible ? ShellAction::Stop : ShellAction::None;
        return appearance;
    }

    return appearance;
}

bool ShellButtonFromCommandId(int command_id, ShellButton& out) noexcept {
    switch (command_id) {
    case kShellButtonIdRecord:
        out = ShellButton::Record;
        return true;
    case kShellButtonIdPauseResume:
        out = ShellButton::PauseResume;
        return true;
    case kShellButtonIdStop:
        out = ShellButton::Stop;
        return true;
    default:
        return false;
    }
}

ShellAction ResolveShellCommand(int command_id, const ShellPresenceState& state) noexcept {
    ShellButton button{};
    if (!ShellButtonFromCommandId(command_id, button))
        return ShellAction::None;

    // Deliberately routed through the same table that draws the strip. A click
    // Explorer delivers from a repaint the shell has not caught up with then
    // resolves against the state that is true now, not the one that drew it.
    const ShellButtonAppearance appearance = ShellButtonFor(button, state);
    if (!appearance.visible || !appearance.enabled)
        return ShellAction::None;
    return appearance.action;
}

} // namespace exosnap
