#include "ShellPresenceAdapter.h"

#include "models/RecordingPulse.h"
#include "notifications/NotificationManager.h"
#include "viewmodels/RecordViewModel.h"

namespace exosnap::quick {

namespace {

// The Saved icon and the Saved toast are two halves of one "it worked", and two
// of those ending at different moments read as a defect. One named constant
// cannot drift the way two numbers can; should the two ever need to differ, that
// is a second named constant with a stated reason.
constexpr int kSavedDwellMs = notifications::NotificationManager::kDismissMs_Saved;

} // namespace

ShellPresenceAdapter::ShellPresenceAdapter(QObject* parent) : QObject(parent) {
    pulse_timer_.setInterval(kRecordingPulseIntervalMs);
    pulse_timer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&pulse_timer_, &QTimer::timeout, this, &ShellPresenceAdapter::onPulseTick);

    saved_timer_.setSingleShot(true);
    saved_timer_.setInterval(kSavedDwellMs);
    QObject::connect(&saved_timer_, &QTimer::timeout, this, [this]() { onSavedDwellExpired(saved_generation_); });

    state_ = ProjectShellPresence(input_);
}

void ShellPresenceAdapter::setRecordingState(UiRecordingState state, bool can_start, bool can_stop, bool can_pause,
                                             bool can_resume, bool has_completed_recording) {
    input_.state = state;
    input_.can_start = can_start;
    input_.can_stop = can_stop;
    input_.can_pause = can_pause;
    input_.can_resume = can_resume;

    // The dwell follows the edges of "there is a finished, successful recording"
    // exactly. A new session clears the flag on its first transition, which is
    // what makes a recording win against a dwell that is still running.
    if (has_completed_recording != had_completed_recording_) {
        had_completed_recording_ = has_completed_recording;
        if (has_completed_recording)
            armSavedDwell();
        else
            clearSavedDwell();
    }

    republish();
}

const ShellPresenceState& ShellPresenceAdapter::presence() const noexcept {
    return state_;
}

bool ShellPresenceAdapter::recording() const noexcept {
    return state_.recording;
}

bool ShellPresenceAdapter::paused() const noexcept {
    return state_.paused;
}

bool ShellPresenceAdapter::busy() const noexcept {
    return state_.busy;
}

bool ShellPresenceAdapter::saved() const noexcept {
    return state_.saved;
}

int ShellPresenceAdapter::pulseFrame() const noexcept {
    return pulse_frame_;
}

bool ShellPresenceAdapter::shellPulseActive() const noexcept {
    return pulse_timer_.isActive();
}

int ShellPresenceAdapter::taskbarPulseLevel() const noexcept {
    return RecordingPulseLevel(pulse_frame_);
}

void ShellPresenceAdapter::setSavedDwellMsForTest(int ms) {
    saved_timer_.setInterval(ms);
}

quint64 ShellPresenceAdapter::savedDwellGenerationForTest() const noexcept {
    return saved_generation_;
}

void ShellPresenceAdapter::expireSavedDwellForTest(quint64 generation) {
    onSavedDwellExpired(generation);
}

void ShellPresenceAdapter::advancePulseForTest() {
    onPulseTick();
}

bool ShellPresenceAdapter::pulseRunningForTest() const {
    return pulse_timer_.isActive();
}

int ShellPresenceAdapter::pulseTicksRemainingForTest() const noexcept {
    return pulse_ticks_remaining_;
}

void ShellPresenceAdapter::armSavedDwell() {
    ++saved_generation_;
    input_.saved_dwell_active = true;
    saved_timer_.start();
}

void ShellPresenceAdapter::clearSavedDwell() {
    ++saved_generation_;
    input_.saved_dwell_active = false;
    saved_timer_.stop();
}

void ShellPresenceAdapter::onSavedDwellExpired(quint64 generation) {
    // A timeout from a dwell that has already been superseded. Qt delivers the
    // queued timeout even after stop() in some orderings, and the recording it
    // would fall back to Idle from is not the one it was armed for.
    if (generation != saved_generation_)
        return;
    if (!input_.saved_dwell_active)
        return;
    input_.saved_dwell_active = false;
    republish();
}

void ShellPresenceAdapter::republish() {
    const ShellPresenceState projected = ProjectShellPresence(input_);
    const bool changed = projected != state_;
    state_ = projected;

    syncPulseTimer();

    if (changed)
        emit presenceChanged();
}

void ShellPresenceAdapter::syncPulseTimer() {
    // The beat marks an ENTRY into Recording, so it is armed on the edge and not
    // by the state being true. Resume is such an edge: re-entering capturing is
    // the same event as entering it, and the shell says so the same way.
    //
    // A countdown does not beat. It shows the recording badge and holds it
    // still, which is what makes "committed" and "capturing" tell apart.
    const bool recording = state_.phase == ShellPhase::Recording;
    if (recording && !was_recording_) {
        was_recording_ = true;
        // Every entry starts at the trough, so the beat is in the same place
        // relative to the recording each time.
        pulse_frame_ = 0;
        pulse_ticks_remaining_ = kRecordingPulseTransitionTicks;
        pulse_timer_.start();
        emit pulseChanged();
        return;
    }
    if (recording)
        return;

    was_recording_ = false;
    if (!pulse_timer_.isActive() && pulse_frame_ == kRecordingPulsePeakFrame && pulse_ticks_remaining_ == 0)
        return;
    // Leaving Recording cancels the beat outright -- Pause, Stop and Finalizing
    // are all static, and a surviving tick would repaint one of them with a
    // recording frame.
    pulse_timer_.stop();
    pulse_ticks_remaining_ = 0;
    pulse_frame_ = kRecordingPulsePeakFrame;
    emit pulseChanged();
}

void ShellPresenceAdapter::onPulseTick() {
    // The countdown is the ONLY guard, and it is zeroed the moment the state
    // leaves Recording. Qt can deliver a queued timeout after stop(), and a tick
    // that repainted a paused tray with a recording frame would leave the shell
    // describing a recording that has stopped -- with nothing left to correct it,
    // because the beat that would have moved on is gone too.
    if (pulse_ticks_remaining_ <= 0)
        return;

    --pulse_ticks_remaining_;
    if (pulse_ticks_remaining_ <= 0) {
        // The transition is over. It ends on the peak, which IS the static
        // recording mark, so the last frame of the beat and the state it settles
        // into are the same image.
        pulse_timer_.stop();
        pulse_frame_ = kRecordingPulsePeakFrame;
    } else {
        pulse_frame_ = NextRecordingPulseFrame(pulse_frame_);
    }
    emit pulseChanged();
}

} // namespace exosnap::quick
