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

// How long finalizing has to last before the shell says so. Above the threshold
// where a changed icon reads as a state rather than as a glitch, and well below
// any remux a user would notice waiting for.
constexpr int kFinalizingSettleMs = 250;

} // namespace

ShellPresenceAdapter::ShellPresenceAdapter(QObject* parent) : QObject(parent) {
    pulse_timer_.setInterval(kRecordingPulseIntervalMs);
    pulse_timer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&pulse_timer_, &QTimer::timeout, this, &ShellPresenceAdapter::onPulseTick);

    processing_timer_.setInterval(kProcessingFrameIntervalMs);
    processing_timer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&processing_timer_, &QTimer::timeout, this, &ShellPresenceAdapter::onProcessingTick);

    finalizing_timer_.setSingleShot(true);
    finalizing_timer_.setInterval(kFinalizingSettleMs);
    QObject::connect(&finalizing_timer_, &QTimer::timeout, this, &ShellPresenceAdapter::onFinalizingSettled);

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

int ShellPresenceAdapter::iconStateValue() const noexcept {
    return static_cast<int>(state_.icon_state);
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

int ShellPresenceAdapter::markFrame() const noexcept {
    switch (state_.icon_state) {
    case ShellIconState::Recording:
        return pulse_frame_;
    case ShellIconState::Processing:
        return processing_frame_;
    default:
        break;
    }
    return 0;
}

bool ShellPresenceAdapter::shellPulseActive() const noexcept {
    return pulse_timer_.isActive();
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

void ShellPresenceAdapter::setFinalizingSettleMsForTest(int ms) {
    finalizing_timer_.setInterval(ms);
}

bool ShellPresenceAdapter::finalizingSettleRunningForTest() const {
    return finalizing_timer_.isActive();
}

void ShellPresenceAdapter::expireFinalizingSettleForTest() {
    onFinalizingSettled();
}

void ShellPresenceAdapter::advanceProcessingForTest() {
    onProcessingTick();
}

bool ShellPresenceAdapter::processingRunningForTest() const {
    return processing_timer_.isActive();
}

void ShellPresenceAdapter::advancePulseForTest() {
    onPulseTick();
}

bool ShellPresenceAdapter::pulseRunningForTest() const {
    return pulse_timer_.isActive();
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

ShellIconState ShellPresenceAdapter::settleIconState(const ShellPresenceState& projected) {
    if (projected.phase != ShellPhase::Finalizing) {
        finalizing_timer_.stop();
        finalizing_settled_ = false;
        return projected.icon_state;
    }
    if (finalizing_settled_)
        return projected.icon_state;
    if (!finalizing_timer_.isActive())
        finalizing_timer_.start();
    // Hold whatever the shell is already showing. Coral, for the ordinary stop.
    return state_.icon_state;
}

void ShellPresenceAdapter::onFinalizingSettled() {
    // Nothing to do if the operation already finished: the phase moved on and the
    // timer's own stop raced this callback.
    if (state_.phase != ShellPhase::Finalizing)
        return;
    finalizing_settled_ = true;
    republish();
}

void ShellPresenceAdapter::republish() {
    ShellPresenceState projected = ProjectShellPresence(input_);
    // The one field the adapter overrides. Everything else -- the phase, the
    // affordances -- stays exactly as the pure projection computed it, so a menu
    // never offers something because an icon is lagging.
    projected.icon_state = settleIconState(projected);
    const bool changed = projected != state_;
    state_ = projected;

    syncPulseTimer();
    syncProcessingTimer();

    if (changed)
        emit presenceChanged();
}

void ShellPresenceAdapter::syncPulseTimer() {
    // The beat runs while the capture does, and not a tick longer. A countdown
    // does not beat: it shows the recording badge and holds it still, which is
    // what makes "committed" and "capturing" tell apart at a glance.
    const bool recording = state_.phase == ShellPhase::Recording;
    if (recording) {
        if (was_recording_)
            return;
        was_recording_ = true;
        // Every entry starts at the bottom of the loop, so the beat sits in the
        // same place relative to the recording each time -- including a resume,
        // which is the same event as an entry.
        pulse_frame_ = kRecordingPulseFirstFrame;
        pulse_timer_.start();
        emit pulseChanged();
        return;
    }

    was_recording_ = false;
    if (!pulse_timer_.isActive() && pulse_frame_ == kRecordingPulseFirstFrame)
        return;
    // Leaving Recording cancels the beat outright -- Pause, Stop and Finalizing
    // are all static, and a surviving tick would repaint one of them with a
    // recording frame.
    pulse_timer_.stop();
    pulse_frame_ = kRecordingPulseFirstFrame;
    emit pulseChanged();
}

void ShellPresenceAdapter::onPulseTick() {
    // The state is the ONLY guard. Qt can deliver a queued timeout after stop(),
    // and a tick that repainted a paused tray with a recording frame would leave
    // the shell describing a recording that has stopped -- with nothing left to
    // correct it, because the beat that would have moved on is gone too.
    if (state_.phase != ShellPhase::Recording)
        return;
    pulse_frame_ = NextRecordingPulseFrame(pulse_frame_);
    emit pulseChanged();
}

void ShellPresenceAdapter::syncProcessingTimer() {
    if (state_.icon_state == ShellIconState::Processing) {
        if (!processing_timer_.isActive()) {
            // Entered from a static mark, so the sequence starts at its first
            // frame rather than wherever the last operation left it.
            processing_frame_ = 0;
            processing_timer_.start();
            emit pulseChanged();
        }
        return;
    }
    if (!processing_timer_.isActive() && processing_frame_ == 0)
        return;
    processing_timer_.stop();
    processing_frame_ = 0;
    emit pulseChanged();
}

void ShellPresenceAdapter::onProcessingTick() {
    // The state is the guard, the way the beat's countdown is: a queued timeout
    // delivered after the operation finished would otherwise repaint a saved or
    // idle mark with a processing frame.
    if (state_.icon_state != ShellIconState::Processing)
        return;
    processing_frame_ = NextRecordingPulseFrame(processing_frame_, kProcessingFrameCount);
    emit pulseChanged();
}

} // namespace exosnap::quick
