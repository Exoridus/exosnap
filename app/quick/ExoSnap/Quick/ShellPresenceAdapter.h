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
    // 0.0 at the trough, 1.0 at the peak, and a flat 0.0 whenever the pulse is
    // not running. An in-app indicator binds its own animation amplitude to this
    // rather than starting a second timer.
    Q_PROPERTY(qreal pulseIntensity READ pulseIntensity NOTIFY pulseChanged FINAL)

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

    // The frame the shell surfaces render. Stable while the pulse is stopped, so
    // a static state always draws the same icon.
    [[nodiscard]] int pulseFrame() const noexcept;
    [[nodiscard]] qreal pulseIntensity() const noexcept;
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
    void advancePulseForTest();
    [[nodiscard]] bool pulseRunningForTest() const;

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

    QTimer saved_timer_;
    // Bumped on every arm AND every clear, which is what makes a timeout that
    // fired after its recording was superseded identifiable. A bare "is the
    // timer still running" check cannot see it: the queued callback outlives the
    // stop.
    quint64 saved_generation_ = 0;
    bool had_completed_recording_ = false;
};

} // namespace exosnap::quick
