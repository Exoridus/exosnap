#pragma once

// The one place the shell's view of the recording session is assembled.
//
// Everything below it -- the tray icon, the window/taskbar icon, the taskbar
// thumbnail buttons, the in-app recording indicator -- reads this object and
// nothing else. What it adds to the pure projection in models/ShellPresence.h is
// the state that needs a clock: the recording-entry heartbeat, the processing
// sequence, the settle that keeps a fast stop from flashing, and the bounded
// dwell after a recording is saved.
//
// Both clocks are owned here rather than by their surfaces. Two timers ticking at
// the same nominal rate drift apart within a minute, and the tray and the taskbar
// would then be describing one recording out of step.

#include <QObject>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/RecordingPulse.h"
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
    // The mark, as an int rather than as a typed enum: models/ShellPresence.h is
    // engine code and has no Qt Quick integration to register, and the only QML
    // that reads this hands it straight back to ui/brand. A surface that turned
    // the number into a decision would be the second state derivation this
    // object exists to prevent.
    Q_PROPERTY(int iconState READ iconStateValue NOTIFY presenceChanged FINAL)
    // The frame of that mark. The in-application mark used to be told there was
    // nothing here to follow, which was true while the shell's beat was a short
    // transition on its own cadence. It is not: the beat now runs for as long as
    // the recording does, and the title band showing a different frame from the
    // tray icon beside it is one recording told two ways.
    Q_PROPERTY(int markFrame READ markFrame NOTIFY pulseChanged FINAL)

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
    // The settled mark -- what the shell SHOWS -- as the underlying value of
    // ShellIconState.
    [[nodiscard]] int iconStateValue() const noexcept;

    // The frame of whichever animated mark the shell is currently showing: the
    // recording beat, the processing sequence, or zero for a static one. One
    // accessor rather than one per animation, because the surfaces render one
    // mark and asking them to pick would be asking them to re-derive the state.
    [[nodiscard]] int markFrame() const noexcept;
    // Whether the recording beat is running. True for as long as the recording
    // is, and false the moment it is not: the beat IS the recording state, not
    // an announcement of having entered it.
    [[nodiscard]] bool shellPulseActive() const noexcept;

    // ---- test seams ------------------------------------------------------
    // The dwell is a wall-clock duration measured in seconds; a test that waited
    // it out would be the slowest in the suite.
    void setSavedDwellMsForTest(int ms);
    [[nodiscard]] quint64 savedDwellGenerationForTest() const noexcept;
    // Runs the dwell's timeout handler with `generation`. Passing a generation
    // that is no longer current is the stale-callback case, and it must change
    // nothing.
    void expireSavedDwellForTest(quint64 generation);
    // Runs one beat tick. The timer is stopped the moment the recording is not
    // running, so a test that keeps calling this is exercising the same guard a
    // queued timeout delivered after a state change hits.
    void advancePulseForTest();
    [[nodiscard]] bool pulseRunningForTest() const;
    // The finalizing settle is a quarter second of wall clock; a test that waited
    // it out would be measuring QTimer.
    void setFinalizingSettleMsForTest(int ms);
    // The processing sequence runs for as long as finalizing lasts, which a test
    // cannot wait out and should not have to.
    void advanceProcessingForTest();
    [[nodiscard]] bool processingRunningForTest() const;
    [[nodiscard]] bool finalizingSettleRunningForTest() const;
    void expireFinalizingSettleForTest();

  signals:
    void presenceChanged();
    void pulseChanged();

  private:
    void armSavedDwell();
    void clearSavedDwell();
    void onSavedDwellExpired(quint64 generation);
    void republish();
    // What the shell surfaces should SHOW, which is not always what the phase
    // says. See the comment on the settle timer below.
    [[nodiscard]] ShellIconState settleIconState(const ShellPresenceState& projected);
    void onFinalizingSettled();
    void syncPulseTimer();
    void onPulseTick();
    void syncProcessingTimer();
    void onProcessingTick();

    ShellPresenceInput input_;
    ShellPresenceState state_;

    QTimer pulse_timer_;
    int pulse_frame_ = kRecordingPulseFirstFrame;

    // Finalizing has no colour of its own -- the recording is over and the file is
    // not there yet -- so it projects to the neutral mark. Painting that the
    // instant a stop begins puts a neutral FLASH between the coral recording and
    // the green result, because for a stream-copy container finalizing is over in
    // well under a tenth of a second. So the neutral mark waits: if the operation
    // finishes first, the shell goes straight from recording to saved, and if it
    // does not, neutral plus the taskbar's progress bar is exactly the right
    // thing to show.
    QTimer finalizing_timer_;
    bool finalizing_settled_ = false;

    // Runs for as long as the shell shows the processing mark. Unbounded on
    // purpose, and bounded in practice by the operation it describes: unlike the
    // recording beat there is no state here that lasts for hours, and the
    // taskbar's progress bar is running beside it for the same reason.
    QTimer processing_timer_;
    int processing_frame_ = 0;

    QTimer saved_timer_;
    // Bumped on every arm AND every clear, which is what makes a timeout that
    // fired after its recording was superseded identifiable. A bare "is the
    // timer still running" check cannot see it: the queued callback outlives the
    // stop.
    quint64 saved_generation_ = 0;
    bool had_completed_recording_ = false;
    // The edge the beat is restarted on. A flag rather than a comparison against
    // the previous state, because republish() runs on cadences that do not
    // change the phase at all.
    bool was_recording_ = false;
};

} // namespace exosnap::quick
