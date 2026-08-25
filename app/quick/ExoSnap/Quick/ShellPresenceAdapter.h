#pragma once

// The one place the shell's view of the recording session is assembled.
//
// Everything below it -- the tray icon, the taskbar overlay badge, the taskbar
// thumbnail buttons, the in-app recording indicator -- reads this object and
// nothing else. What it adds to the pure projection in models/ShellPresence.h is
// the two pieces of state that need a clock: the recording heartbeat, and the
// bounded dwell after a recording is saved.
//
// Both clocks are owned here rather than by their surfaces. Three timers ticking
// at the same nominal rate drift apart within a minute, and the tray and the
// taskbar would then be describing one recording out of step.

#include <QObject>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/ShellPresence.h"

namespace exosnap {
enum class UiRecordingState;
}

namespace exosnap::quick {

class ShellPresenceAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("ShellPresenceAdapter is provided by the application")

    Q_PROPERTY(bool recording READ recording NOTIFY presenceChanged FINAL)
    Q_PROPERTY(bool paused READ paused NOTIFY presenceChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY presenceChanged FINAL)
    Q_PROPERTY(bool saved READ saved NOTIFY presenceChanged FINAL)
    // Deliberately no pulse property. The application's own recording indicator
    // animates itself for as long as the recording runs; the shell's beat is a
    // short transition on a different cadence, and a QML surface following it
    // would be following the wrong one.

  public:
    explicit ShellPresenceAdapter(QObject* parent = nullptr);

    // The single edge. Called from the one function that already runs on every
    // recording-state change, with the view model's own Can* answers rather than
    // a second derivation of them.
    //
    // `has_completed_recording` is what arms the Saved dwell: it is true only
    // for a result that both finished and succeeded, so a failure can never be
    // reported green.
    void setRecordingState(UiRecordingState state, bool can_start, bool can_stop, bool can_pause, bool can_resume,
                           bool has_completed_recording);

    [[nodiscard]] const ShellPresenceState& presence() const noexcept;

    [[nodiscard]] bool recording() const noexcept;
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool saved() const noexcept;

    // The frame the shell surfaces render. After the entry beat it rests on the
    // peak, so a static recording always draws the mark at full weight.
    [[nodiscard]] int pulseFrame() const noexcept;
    // Whether the recording-entry beat is still playing. It ends on its own after
    // a fixed couple of cycles, which is what makes the shell go static.
    [[nodiscard]] bool shellPulseActive() const noexcept;
    // The same phase quantized for the taskbar overlay, which is redrawn by
    // Explorer and does not want every frame.
    [[nodiscard]] int taskbarPulseLevel() const noexcept;

    // ---- test seams ------------------------------------------------------
    // The dwell is a wall-clock duration measured in seconds; a test that waited
    // it out would be the slowest in the suite.
    void setSavedDwellMsForTest(int ms);
    [[nodiscard]] quint64 savedDwellGenerationForTest() const noexcept;
    // Runs the dwell's timeout handler with `generation`. Passing a generation
    // that is no longer current is the stale-callback case, and it must change
    // nothing.
    void expireSavedDwellForTest(quint64 generation);
    // Runs one beat tick. The real timer is stopped as soon as the transition
    // ends, so a test that keeps calling this is exercising the same guard a
    // queued timeout delivered after a state change hits.
    void advancePulseForTest();
    [[nodiscard]] bool pulseRunningForTest() const;
    [[nodiscard]] int pulseTicksRemainingForTest() const noexcept;

  signals:
    void presenceChanged();
    void pulseChanged();

  private:
    void armSavedDwell();
    void clearSavedDwell();
    void onSavedDwellExpired(quint64 generation);
    void republish();
    void syncPulseTimer();
    void onPulseTick();

    ShellPresenceInput input_;
    ShellPresenceState state_;

    QTimer pulse_timer_;
    int pulse_frame_ = 0;
    // Counts the entry beat down. Zero means the shell is showing a static
    // state, whether or not a recording is running.
    int pulse_ticks_remaining_ = 0;

    QTimer saved_timer_;
    // Bumped on every arm AND every clear, which is what makes a timeout that
    // fired after its recording was superseded identifiable. A bare "is the
    // timer still running" check cannot see it: the queued callback outlives the
    // stop.
    quint64 saved_generation_ = 0;
    bool had_completed_recording_ = false;
    // The edge the entry beat is armed on. A flag rather than a comparison
    // against the previous state, because republish() runs on cadences that do
    // not change the phase at all.
    bool was_recording_ = false;
};

} // namespace exosnap::quick
