#include "ShellPresenceAdapter.h"
#include "TaskbarPresence.h"
#include "TrayAdapter.h"
#include "models/RecordingPulse.h"
#include "models/ShellPresence.h"
#include "models/TaskbarProgressLease.h"
#include "viewmodels/RecordViewModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSet>
#include <QSignalSpy>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using exosnap::kProcessingFrameCount;
using exosnap::kProcessingFrameIntervalMs;
using exosnap::kRecordingPulseFirstFrame;
using exosnap::kRecordingPulseFrameCount;
using exosnap::kRecordingPulseIntervalMs;
using exosnap::kShellButtonIdPauseResume;
using exosnap::kShellButtonIdRecord;
using exosnap::kShellButtonIdStop;
using exosnap::ProjectShellPresence;
using exosnap::ShellAction;
using exosnap::ShellIconState;
using exosnap::ShellPhase;
using exosnap::ShellPresenceInput;
using exosnap::ShellPresenceState;
using exosnap::TaskbarProgressOwner;
using exosnap::TaskbarProgressState;
using exosnap::UiRecordingState;
using exosnap::quick::kThumbButtonClickedNotification;
using exosnap::quick::ShellPresenceAdapter;
using exosnap::quick::TaskbarPresence;
using exosnap::quick::TaskbarShell;
using exosnap::quick::ThumbButtonSpec;
using exosnap::quick::TrayAdapter;

namespace {

// Both the pulse and the Saved dwell are QTimers, which need an event loop to
// exist at all.
QCoreApplication* EnsureApplication() {
    if (QCoreApplication::instance() != nullptr)
        return QCoreApplication::instance();
    static int argc = 1;
    static char app_name[] = "shell_surface_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

// A WM_COMMAND wParam exactly as Explorer composes it for a thumbnail button
// click: THBN_CLICKED in the high word, the button id in the low word.
constexpr quint64 Click(int command_id) {
    return (static_cast<quint64>(kThumbButtonClickedNotification) << 16) | static_cast<quint64>(command_id);
}

// Two arbitrary non-null values standing in for HWNDs. Their only requirement is
// that they differ, which is what makes an identity change observable.
void* const kHandleA = reinterpret_cast<void*>(0x1000);
void* const kHandleB = reinterpret_cast<void*>(0x2000);

constexpr qint32 kOk = 0;
// An arbitrary failing HRESULT. E_FAIL, spelled out so this file needs no
// windows.h.
constexpr qint32 kFail = static_cast<qint32>(0x80004005U);

ShellPresenceState PresenceFor(UiRecordingState state) {
    ShellPresenceInput input;
    input.state = state;
    input.can_start =
        state == UiRecordingState::Ready || state == UiRecordingState::Completed || state == UiRecordingState::Failed;
    input.can_stop = state == UiRecordingState::Recording || state == UiRecordingState::Paused;
    input.can_pause = state == UiRecordingState::Recording;
    input.can_resume = state == UiRecordingState::Paused;
    return ProjectShellPresence(input);
}

// Records every platform call and can be made to refuse any one of them.
class FakeTaskbarShell : public TaskbarShell {
  public:
    struct Log {
        int initialize = 0;
        int add_buttons = 0;
        int update_buttons = 0;
        int progress_state = 0;
        int progress_value = 0;
    };

    struct Failures {
        bool initialize = false;
        bool add_buttons = false;
        bool update_buttons = false;
        bool progress = false;
    };

    Log log;
    Failures fail;
    QVector<ThumbButtonSpec> last_buttons;
    TaskbarProgressState last_progress_state = TaskbarProgressState::NoProgress;
    quint64 last_completed = 0;
    quint64 last_total = 0;
    std::vector<void*> handles;

    qint32 initialize() override {
        ++log.initialize;
        return fail.initialize ? kFail : kOk;
    }
    qint32 addButtons(void* hwnd, const QVector<ThumbButtonSpec>& buttons) override {
        ++log.add_buttons;
        handles.push_back(hwnd);
        last_buttons = buttons;
        return fail.add_buttons ? kFail : kOk;
    }
    qint32 updateButtons(void* hwnd, const QVector<ThumbButtonSpec>& buttons) override {
        ++log.update_buttons;
        handles.push_back(hwnd);
        last_buttons = buttons;
        return fail.update_buttons ? kFail : kOk;
    }
    qint32 setProgressState(void* hwnd, TaskbarProgressState state) override {
        ++log.progress_state;
        handles.push_back(hwnd);
        last_progress_state = state;
        return fail.progress ? kFail : kOk;
    }
    qint32 setProgressValue(void* hwnd, quint64 completed, quint64 total) override {
        ++log.progress_value;
        handles.push_back(hwnd);
        last_completed = completed;
        last_total = total;
        return fail.progress ? kFail : kOk;
    }
};

class TaskbarPresenceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        EnsureApplication();
        auto shell = std::make_unique<FakeTaskbarShell>();
        shell_ = shell.get();
        presence_.setShellForTest(std::move(shell));
    }

    // The ordinary startup order: the window exists, then Explorer announces the
    // button.
    void bringUp(void* hwnd = kHandleA) {
        presence_.setHandle(hwnd);
        presence_.notifyShellReady(hwnd);
    }

    TaskbarPresence presence_;
    FakeTaskbarShell* shell_ = nullptr;
};

// Spins the event loop until `predicate` holds or the budget runs out, so a
// timer-backed assertion neither sleeps for its full duration nor hangs.
template <typename Predicate> bool SpinUntil(Predicate predicate, int budget_ms = 2000) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate()) {
        if (clock.elapsed() > budget_ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

} // namespace

// -- ShellPresenceAdapter: what QML can see -----------------------------------

TEST(ShellPresenceAdapterQml, TheMarkAndItsFrameReachQmlAsProperties) {
    // Read through the meta-object rather than through the accessors: the Top Bar
    // binds to `iconState` and `markFrame` by name, and an accessor that exists
    // without a Q_PROPERTY beside it is a mark that never changes.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);

    EXPECT_EQ(adapter.property("iconState").toInt(), static_cast<int>(ShellIconState::Recording));
    EXPECT_EQ(adapter.property("markFrame").toInt(), adapter.markFrame());

    const int before = adapter.property("markFrame").toInt();
    adapter.advancePulseForTest();
    EXPECT_NE(adapter.property("markFrame").toInt(), before);
}

TEST(ShellPresenceAdapterQml, TheStateQmlSeesIsTheSettledOneTheShellShows) {
    // Not the raw phase. A stop that finalizes instantly must not put a neutral
    // flash in the title band any more than it may put one in the tray, and the
    // way to guarantee that is for both to read the same property.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setFinalizingSettleMsForTest(10000);
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Stopping, false, false, false, false, false);

    EXPECT_EQ(adapter.property("iconState").toInt(), static_cast<int>(ShellIconState::Recording));
    adapter.expireFinalizingSettleForTest();
    EXPECT_EQ(adapter.property("iconState").toInt(), static_cast<int>(adapter.presence().icon_state));
}

TEST(ShellPresenceAdapterQml, AStateChangeNotifiesTheBinding) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Ready, true, false, false, false, false);
    QSignalSpy spy(&adapter, &ShellPresenceAdapter::presenceChanged);
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(adapter.property("iconState").toInt(), static_cast<int>(ShellIconState::Recording));
}

// -- ShellPresenceAdapter: pulse --------------------------------------------

TEST(ShellPresenceAdapterPulse, RunsOnlyWhileRecording) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Ready, true, false, false, false, false);
    EXPECT_FALSE(adapter.pulseRunningForTest());

    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    EXPECT_TRUE(adapter.pulseRunningForTest());

    adapter.setRecordingState(UiRecordingState::Paused, false, true, false, true, false);
    EXPECT_FALSE(adapter.pulseRunningForTest());
}

TEST(ShellPresenceAdapterPulse, ACountdownShowsTheRecordingBadgeButHoldsItStill) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Countdown, false, false, false, false, false);
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Recording);
    EXPECT_FALSE(adapter.pulseRunningForTest());
}

TEST(ShellPresenceAdapterPulse, TheBeatRunsForAsLongAsTheRecordingDoes) {
    // Not a transition any more. The frames modulate brightness only, which has
    // no sub-pixel problem at 16 px, so the mark can keep beating instead of
    // announcing the state change and then going still.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    ASSERT_TRUE(adapter.shellPulseActive());

    for (int tick = 0; tick < kRecordingPulseFrameCount * 5; ++tick) {
        adapter.advancePulseForTest();
        EXPECT_TRUE(adapter.shellPulseActive()) << "the beat stopped at tick " << tick;
    }
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Recording);
}

TEST(ShellPresenceAdapterPulse, AStaticMarkHasNoFrameAtAll) {
    // Leaving Recording cancels the beat, and the mark the shell then shows is a
    // single drawing. Reporting a leftover frame number for it would put a
    // recording frame's index in a paused icon's URL.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.advancePulseForTest();

    adapter.setRecordingState(UiRecordingState::Paused, false, true, false, true, false);
    EXPECT_FALSE(adapter.shellPulseActive());
    EXPECT_EQ(adapter.markFrame(), 0);
}

TEST(ShellPresenceAdapterPulse, EveryRecordingStartsAtTheBottomOfTheLoop) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.advancePulseForTest();
    ASSERT_NE(adapter.markFrame(), kRecordingPulseFirstFrame);

    adapter.setRecordingState(UiRecordingState::Ready, true, false, false, false, false);
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    EXPECT_EQ(adapter.markFrame(), kRecordingPulseFirstFrame);
}

TEST(ShellPresenceAdapterPulse, ThePhaseWrapsThroughTheWholeBeat) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    QSet<int> seen;
    for (int i = 0; i < kRecordingPulseFrameCount; ++i) {
        seen.insert(adapter.markFrame());
        adapter.advancePulseForTest();
    }
    EXPECT_EQ(seen.size(), kRecordingPulseFrameCount);
    EXPECT_EQ(adapter.markFrame(), kRecordingPulseFirstFrame);
}

TEST(ShellPresenceAdapterPulse, TheMetricsCadenceDoesNotRestartTheBeat) {
    // republish() runs several times a second on state that has not changed. A
    // beat rearmed by each of those would sit on its first frame forever.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.advancePulseForTest();
    adapter.advancePulseForTest();
    const int frame = adapter.markFrame();
    ASSERT_NE(frame, kRecordingPulseFirstFrame);

    for (int i = 0; i < 5; ++i)
        adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    EXPECT_TRUE(adapter.shellPulseActive());
    EXPECT_EQ(adapter.markFrame(), frame) << "the beat restarted on a republish";
}

TEST(ShellPresenceAdapterPulse, ResumeStartsTheBeatAgainFromTheBottom) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.advancePulseForTest();

    adapter.setRecordingState(UiRecordingState::Paused, false, true, false, true, false);
    ASSERT_FALSE(adapter.shellPulseActive());
    // Re-entering capturing is the same event as entering it.
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    EXPECT_TRUE(adapter.shellPulseActive());
    EXPECT_EQ(adapter.markFrame(), kRecordingPulseFirstFrame);
}

TEST(ShellPresenceAdapterPulse, StoppingCancelsTheBeat) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.advancePulseForTest();
    ASSERT_TRUE(adapter.shellPulseActive());

    adapter.setRecordingState(UiRecordingState::Stopping, false, false, false, false, false);
    EXPECT_FALSE(adapter.shellPulseActive());
    EXPECT_EQ(adapter.markFrame(), 0);
}

TEST(ShellPresenceAdapterPulse, AStalePulseTickCannotRepaintALaterState) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.advancePulseForTest();
    adapter.setRecordingState(UiRecordingState::Paused, false, true, false, true, false);
    const int settled = adapter.markFrame();

    // The tick Qt had already queued when the state changed.
    adapter.advancePulseForTest();
    EXPECT_EQ(adapter.markFrame(), settled);
    EXPECT_FALSE(adapter.shellPulseActive());
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Paused);
}

TEST(ShellPresenceAdapterPulse, TheTimerActuallyTicksOnItsOwn) {
    // The seam above drives the handler directly; this is the counter-check that
    // there is a timer behind it at all.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    ASSERT_EQ(adapter.markFrame(), 0);
    EXPECT_TRUE(SpinUntil([&adapter]() { return adapter.markFrame() != 0; }));
}

// -- ShellPresenceAdapter: the finalizing flash ------------------------------
//
// Finalizing projects to the processing mark, and for a stream-copy container it
// is over in well under a tenth of a second. Painting it straight away put a
// FLASH between the coral recording and the green result on both shell surfaces.

TEST(ShellPresenceAdapterFinalizing, AShortStopNeverShowsTheProcessingMark) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Recording);

    adapter.setRecordingState(UiRecordingState::Stopping, false, false, false, false, false);
    // The PHASE is honest -- the transport knows the session is finalizing --
    // while the ICON holds what it had.
    EXPECT_EQ(adapter.presence().phase, ShellPhase::Finalizing);
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Recording);

    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Saved);
}

TEST(ShellPresenceAdapterFinalizing, AFinalizingThatLastsIsShown) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Saving, false, false, false, false, false);
    ASSERT_TRUE(adapter.finalizingSettleRunningForTest());

    // A remux long enough to be worth reporting: the processing mark, and the
    // taskbar's progress bar beside it.
    adapter.expireFinalizingSettleForTest();
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Processing);
    EXPECT_EQ(adapter.presence().phase, ShellPhase::Finalizing);
}

TEST(ShellPresenceAdapterFinalizing, LeavingFinalizingCancelsTheSettle) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Stopping, false, false, false, false, false);
    ASSERT_TRUE(adapter.finalizingSettleRunningForTest());

    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    EXPECT_FALSE(adapter.finalizingSettleRunningForTest());
}

TEST(ShellPresenceAdapterFinalizing, AStaleSettleCallbackCannotRepaintALaterState) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Stopping, false, false, false, false, false);
    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Saved);

    // The timeout Qt had already queued when the stop finished.
    adapter.expireFinalizingSettleForTest();
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Saved);
}

TEST(ShellPresenceAdapterFinalizing, ASecondFinalizingSettlesAgainFromScratch) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Saving, false, false, false, false, false);
    adapter.expireFinalizingSettleForTest();
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Processing);

    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Stopping, false, false, false, false, false);
    // Not still "settled" from the previous recording.
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Recording);
    EXPECT_TRUE(adapter.finalizingSettleRunningForTest());
}

TEST(ShellPresenceAdapterFinalizing, TheSettleTimerActuallyRunsOnItsOwn) {
    // The seam above drives the handler directly; this is the counter-check that
    // there is a timer behind it.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setFinalizingSettleMsForTest(10);
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Saving, false, false, false, false, false);
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Recording);
    EXPECT_TRUE(SpinUntil([&adapter]() { return adapter.presence().icon_state == ShellIconState::Processing; }));
}

// -- ShellPresenceAdapter: saved dwell ---------------------------------------

// -- ShellPresenceAdapter: the processing sequence ---------------------------
//
// Unlike the recording beat this one is not a transition: it runs for as long as
// the operation it describes does. What bounds it is the operation, and the
// taskbar's progress bar is running beside it for the same reason.

TEST(ShellPresenceAdapterProcessing, TheSequenceWaitsForTheSettle) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Saving, false, false, false, false, false);
    // The mark is still the recording one, so animating would animate THAT.
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Recording);
    EXPECT_FALSE(adapter.processingRunningForTest());

    adapter.expireFinalizingSettleForTest();
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Processing);
    EXPECT_TRUE(adapter.processingRunningForTest());
    EXPECT_EQ(adapter.markFrame(), 0) << "the sequence starts at its first frame";
}

TEST(ShellPresenceAdapterProcessing, TheSequenceAdvancesAndWraps) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Saving, false, false, false, false, false);
    adapter.expireFinalizingSettleForTest();
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Processing);

    QSet<int> seen;
    for (int tick = 0; tick < kProcessingFrameCount * 2; ++tick) {
        seen.insert(adapter.markFrame());
        adapter.advanceProcessingForTest();
    }
    EXPECT_EQ(seen.size(), kProcessingFrameCount);
    EXPECT_EQ(adapter.markFrame(), 0) << "two full cycles land back on the first frame";
}

TEST(ShellPresenceAdapterProcessing, LeavingProcessingStopsTheSequence) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Saving, false, false, false, false, false);
    adapter.expireFinalizingSettleForTest();
    adapter.advanceProcessingForTest();
    ASSERT_TRUE(adapter.processingRunningForTest());

    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    EXPECT_FALSE(adapter.processingRunningForTest());
    EXPECT_EQ(adapter.markFrame(), 0) << "a static mark has no frame";
}

TEST(ShellPresenceAdapterProcessing, AStaleTickCannotRepaintALaterState) {
    // Qt delivers a queued timeout even after stop(), and a tick that moved the
    // frame under a saved mark would put a processing frame's URL on a green one.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Saving, false, false, false, false, false);
    adapter.expireFinalizingSettleForTest();
    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Saved);

    adapter.advanceProcessingForTest();
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Saved);
    EXPECT_EQ(adapter.markFrame(), 0);
}

TEST(ShellPresenceAdapterProcessing, TheTimerActuallyRunsOnItsOwn) {
    // The seam above drives the handler directly; this is the counter-check that
    // there is a real timer behind it.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Saving, false, false, false, false, false);
    adapter.expireFinalizingSettleForTest();
    ASSERT_EQ(adapter.markFrame(), 0);
    EXPECT_TRUE(SpinUntil([&adapter]() { return adapter.markFrame() != 0; }, 4000));
}

TEST(ShellPresenceAdapterProcessing, TheTwoSequencesShareOneCadenceAndNotOneLength) {
    // One tick rate, so a stop that crosses from one sequence to the other does
    // not change pace. Different lengths on purpose: the recording loop rests at
    // the bottom, and a spinner that paused would read as a stalled operation.
    EXPECT_EQ(kProcessingFrameIntervalMs, kRecordingPulseIntervalMs);
    EXPECT_EQ(kRecordingPulseIntervalMs, 250);
    EXPECT_NE(kProcessingFrameCount, kRecordingPulseFrameCount);
}

TEST(ShellPresenceAdapterSaved, ASuccessfulRecordingTurnsTheBadgeGreenForADwell) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    EXPECT_TRUE(adapter.saved());
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Saved);
}

TEST(ShellPresenceAdapterSaved, AFailedRecordingIsNeverGreen) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Failed, true, false, false, false, false);
    EXPECT_FALSE(adapter.saved());
    // Its own mark rather than the neutral one: a failure the user has not seen
    // yet is exactly what a shell surface is for, and it is not "idle".
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Error);
    EXPECT_TRUE(adapter.presence().failed);
}

TEST(ShellPresenceAdapterSaved, AFailureIsNotAStateTheUserIsStuckIn) {
    // Unbounded on purpose -- unlike the Saved dwell there is no good moment to
    // stop reporting it -- but a new session clears it on its first transition.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Failed, true, false, false, false, false);
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Error);
    EXPECT_TRUE(adapter.presence().can_start) << "a failed recording must still offer a new one";

    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Recording);
    EXPECT_FALSE(adapter.presence().failed);
}

TEST(ShellPresenceAdapterSaved, TheDwellExpiresBackToIdle) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setSavedDwellMsForTest(1);
    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    ASSERT_TRUE(adapter.saved());
    EXPECT_TRUE(SpinUntil([&adapter]() { return !adapter.saved(); }));
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Idle);
}

TEST(ShellPresenceAdapterSaved, ANewRecordingWinsAgainstARunningDwell) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    ASSERT_TRUE(adapter.saved());

    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    EXPECT_FALSE(adapter.saved());
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Recording);
}

TEST(ShellPresenceAdapterSaved, AStaleTimeoutCannotOverwriteTheRecordingThatSupersededIt) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    const quint64 stale = adapter.savedDwellGenerationForTest();

    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    ASSERT_EQ(adapter.presence().icon_state, ShellIconState::Recording);

    adapter.expireSavedDwellForTest(stale);
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Recording);
    EXPECT_TRUE(adapter.pulseRunningForTest());
}

TEST(ShellPresenceAdapterSaved, TheCurrentGenerationDoesExpireTheDwell) {
    // The counter-check for the assertion above: the guard must reject a stale
    // generation and nothing else.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    ASSERT_TRUE(adapter.saved());
    adapter.expireSavedDwellForTest(adapter.savedDwellGenerationForTest());
    EXPECT_FALSE(adapter.saved());
}

TEST(ShellPresenceAdapterSaved, EachDwellGetsItsOwnGeneration) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    const quint64 first = adapter.savedDwellGenerationForTest();
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.setRecordingState(UiRecordingState::Completed, true, false, false, false, true);
    EXPECT_NE(adapter.savedDwellGenerationForTest(), first);
}

TEST(ShellPresenceAdapterSaved, PresenceChangedFiresOnlyOnARealChange) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    QSignalSpy spy(&adapter, &ShellPresenceAdapter::presenceChanged);
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    ASSERT_EQ(spy.count(), 1);
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    EXPECT_EQ(spy.count(), 1);
}

// -- TaskbarPresence: lifecycle ---------------------------------------------

TEST_F(TaskbarPresenceTest, NothingIsSentBeforeTheShellSaysItIsReady) {
    presence_.setHandle(kHandleA);
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));

    EXPECT_FALSE(presence_.ready());
    EXPECT_EQ(shell_->log.add_buttons, 0);
    EXPECT_EQ(shell_->log.update_buttons, 0);
}

TEST_F(TaskbarPresenceTest, ReadinessForAnotherWindowIsIgnored) {
    presence_.setHandle(kHandleA);
    presence_.notifyShellReady(kHandleB);
    EXPECT_FALSE(presence_.ready());
    EXPECT_EQ(shell_->log.add_buttons, 0);
}

TEST_F(TaskbarPresenceTest, ReadinessArmsTheCurrentHandleAndRegistersTheFullSet) {
    bringUp();
    EXPECT_TRUE(presence_.ready());
    EXPECT_TRUE(presence_.shellAvailable());
    EXPECT_TRUE(presence_.buttonsRegistered());
    EXPECT_EQ(shell_->log.add_buttons, 1);
    // The set is fixed after registration, so all three slots go up front.
    EXPECT_EQ(shell_->last_buttons.size(), 4);
}

TEST_F(TaskbarPresenceTest, ARepeatedAnnouncementReArmsWithoutDuplicatingTheSet) {
    // Explorer broadcasts TaskbarButtonCreated again after it restarts, and the
    // button it announces then is a NEW one with none of this window's
    // registrations. Ignoring the repeat would cost the user the thumbnail
    // transport for the rest of the session -- so it re-arms, and the set it
    // registers is still exactly the same three slots.
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    ASSERT_EQ(shell_->log.add_buttons, 1);

    presence_.notifyShellReady(kHandleA);
    EXPECT_EQ(shell_->log.add_buttons, 2);
    EXPECT_EQ(shell_->last_buttons.size(), 4);
    EXPECT_TRUE(presence_.buttonsRegistered());
    // The interface behind it is re-created: a proxy to a dead Explorer is not
    // reusable.
    EXPECT_EQ(shell_->log.initialize, 2);
}

TEST_F(TaskbarPresenceTest, ARunningOperationIsRePublishedToTheNewTaskbarButton) {
    bringUp();
    const auto lease = presence_.acquireProgress(TaskbarProgressOwner::EditExport);
    presence_.updateProgress(lease, 0.35);
    shell_->last_progress_state = TaskbarProgressState::NoProgress;
    shell_->last_completed = 0;

    presence_.notifyShellReady(kHandleA);
    EXPECT_EQ(shell_->last_progress_state, TaskbarProgressState::Normal);
    EXPECT_EQ(shell_->last_completed, 35u);
}

TEST_F(TaskbarPresenceTest, TheDesiredStateIsReAppliedOnceTheShellIsReady) {
    presence_.setHandle(kHandleA);
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    ASSERT_EQ(shell_->log.add_buttons, 0);

    presence_.notifyShellReady(kHandleA);
    ASSERT_EQ(shell_->last_buttons.size(), 4);
    EXPECT_FALSE(shell_->last_buttons[0].visible); // Record
    EXPECT_TRUE(shell_->last_buttons[1].visible);  // Pause
    EXPECT_TRUE(shell_->last_buttons[2].visible);  // Stop
}

TEST_F(TaskbarPresenceTest, AHandleIdentityChangeDropsReadinessAndTheAppliedState) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    ASSERT_TRUE(presence_.ready());

    presence_.setHandle(kHandleB);
    EXPECT_FALSE(presence_.ready());
    EXPECT_FALSE(presence_.buttonsRegistered());

    // A new taskbar button has been told nothing, so nothing may be sent to it.
    const int before = shell_->log.update_buttons + shell_->log.add_buttons;
    presence_.setPresence(PresenceFor(UiRecordingState::Paused));
    EXPECT_EQ(shell_->log.update_buttons + shell_->log.add_buttons, before);
}

TEST_F(TaskbarPresenceTest, TheNewHandleNeedsItsOwnReadinessAndThenGetsTheFullState) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    presence_.setHandle(kHandleB);
    presence_.setPresence(PresenceFor(UiRecordingState::Paused));

    presence_.notifyShellReady(kHandleB);
    EXPECT_TRUE(presence_.ready());
    EXPECT_TRUE(presence_.buttonsRegistered());
    ASSERT_EQ(shell_->last_buttons.size(), 4);
    // Paused: Record hidden, Resume and Stop offered.
    EXPECT_FALSE(shell_->last_buttons[0].visible);
    EXPECT_TRUE(shell_->last_buttons[1].visible);
    EXPECT_EQ(shell_->handles.back(), kHandleB);
}

TEST_F(TaskbarPresenceTest, TheSameHandleAgainChangesNothing) {
    bringUp();
    const int add_buttons = shell_->log.add_buttons;
    presence_.setHandle(kHandleA);
    EXPECT_TRUE(presence_.ready());
    EXPECT_EQ(shell_->log.add_buttons, add_buttons);
}

TEST_F(TaskbarPresenceTest, AnUnchangedStateCostsNoShellCall) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    const int updates = shell_->log.update_buttons;

    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    EXPECT_EQ(shell_->log.update_buttons, updates);
}

TEST_F(TaskbarPresenceTest, RecordButtonStartsFromIdle) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Ready));
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdRecord)));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ShellAction>(), ShellAction::Start);
}

TEST_F(TaskbarPresenceTest, TheSharedButtonPausesThenResumes) {
    bringUp();
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);

    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdPauseResume)));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ShellAction>(), ShellAction::Pause);

    presence_.setPresence(PresenceFor(UiRecordingState::Paused));
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdPauseResume)));
    ASSERT_EQ(spy.count(), 2);
    EXPECT_EQ(spy.at(1).at(0).value<ShellAction>(), ShellAction::Resume);
}

TEST_F(TaskbarPresenceTest, StopButtonStops) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdStop)));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ShellAction>(), ShellAction::Stop);
}

TEST_F(TaskbarPresenceTest, AnUnknownCommandIsNotOursAndAsksForNothing) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);
    EXPECT_FALSE(presence_.handleCommand(Click(0x1234)));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(TaskbarPresenceTest, AWmCommandThatIsNotAThumbnailClickIsNotOurs) {
    // The filter is process-wide, and WM_COMMAND carries menu and accelerator
    // notifications too. Only THBN_CLICKED in the high word makes it ours.
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);
    EXPECT_FALSE(presence_.handleCommand(static_cast<quint64>(kShellButtonIdStop)));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(TaskbarPresenceTest, ACommandFromAStaleStripIsConsumedAndDoesNothing) {
    // Explorer delivers the click against the strip it last painted, which can
    // be a state the session has already left.
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Saving));
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);

    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdStop)));
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdPauseResume)));
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdRecord)));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(TaskbarPresenceTest, TheStripAClickIsResolvedAgainstIsTheStripThatWasSent) {
    bringUp();
    const ShellPresenceState recording = PresenceFor(UiRecordingState::Recording);
    presence_.setPresence(recording);
    const QVector<ThumbButtonSpec> expected = TaskbarPresence::ButtonsFor(recording);
    ASSERT_EQ(shell_->last_buttons.size(), expected.size());
    for (int i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(shell_->last_buttons[i].command_id, expected[i].command_id);
        EXPECT_EQ(shell_->last_buttons[i].visible, expected[i].visible);
        EXPECT_EQ(shell_->last_buttons[i].enabled, expected[i].enabled);
        EXPECT_EQ(shell_->last_buttons[i].action, expected[i].action);
    }
}

// -- TaskbarPresence: progress ----------------------------------------------

TEST_F(TaskbarPresenceTest, ProgressPublishesThroughTheLease) {
    bringUp();
    const auto lease = presence_.acquireProgress(TaskbarProgressOwner::RecordingSave);
    ASSERT_TRUE(lease.valid());
    EXPECT_EQ(shell_->last_progress_state, TaskbarProgressState::Indeterminate);

    presence_.updateProgress(lease, 0.25);
    EXPECT_EQ(shell_->last_progress_state, TaskbarProgressState::Normal);
    EXPECT_EQ(shell_->last_completed, 25u);
    EXPECT_EQ(shell_->last_total, 100u);

    presence_.finishProgress(lease);
    EXPECT_EQ(shell_->last_progress_state, TaskbarProgressState::NoProgress);
}

TEST_F(TaskbarPresenceTest, AFailedOperationLeavesTheErrorOnTheBar) {
    bringUp();
    const auto lease = presence_.acquireProgress(TaskbarProgressOwner::EditExport);
    presence_.failProgress(lease);
    EXPECT_EQ(shell_->last_progress_state, TaskbarProgressState::Error);
    EXPECT_FALSE(presence_.progress().held());
}

TEST_F(TaskbarPresenceTest, ASecondProducerCannotCorruptTheFirst) {
    bringUp();
    const auto first = presence_.acquireProgress(TaskbarProgressOwner::RecordingSave);
    presence_.updateProgress(first, 0.6);

    const auto second = presence_.acquireProgress(TaskbarProgressOwner::EditExport);
    EXPECT_FALSE(second.valid());

    const int values = shell_->log.progress_value;
    presence_.updateProgress(second, 0.1);
    presence_.failProgress(second);
    EXPECT_EQ(shell_->log.progress_value, values);
    EXPECT_EQ(shell_->last_progress_state, TaskbarProgressState::Normal);
    EXPECT_DOUBLE_EQ(presence_.progress().fraction(), 0.6);
}

TEST_F(TaskbarPresenceTest, AStaleOwnerCallbackCannotMoveTheNextOwnersBar) {
    bringUp();
    const auto stale = presence_.acquireProgress(TaskbarProgressOwner::RecordingSave);
    presence_.finishProgress(stale);
    const auto current = presence_.acquireProgress(TaskbarProgressOwner::EditExport);
    presence_.updateProgress(current, 0.2);

    presence_.updateProgress(stale, 0.9);
    EXPECT_EQ(shell_->last_completed, 20u);
    presence_.cancelProgress(stale);
    EXPECT_TRUE(presence_.progress().held());
}

TEST_F(TaskbarPresenceTest, TeardownReleasesTheBar) {
    bringUp();
    const auto lease = presence_.acquireProgress(TaskbarProgressOwner::RecoveryFinish);
    presence_.releaseProgress(lease);
    EXPECT_FALSE(presence_.progress().held());
    EXPECT_EQ(shell_->last_progress_state, TaskbarProgressState::NoProgress);
}

TEST_F(TaskbarPresenceTest, ProgressTakenBeforeReadinessIsPublishedWhenTheShellArrives) {
    presence_.setHandle(kHandleA);
    const auto lease = presence_.acquireProgress(TaskbarProgressOwner::RecordingSave);
    ASSERT_TRUE(lease.valid());
    presence_.updateProgress(lease, 0.4);
    ASSERT_EQ(shell_->log.progress_value, 0);

    presence_.notifyShellReady(kHandleA);
    EXPECT_EQ(shell_->last_progress_state, TaskbarProgressState::Normal);
    EXPECT_EQ(shell_->last_completed, 40u);
}

// -- TaskbarPresence: failure semantics --------------------------------------

TEST_F(TaskbarPresenceTest, ARefusedInitializationLeavesEverythingElseWorking) {
    shell_->fail.initialize = true;
    bringUp();

    EXPECT_FALSE(presence_.shellAvailable());
    EXPECT_FALSE(presence_.buttonsRegistered());
    EXPECT_EQ(shell_->log.add_buttons, 0);

    // And the product state still tracks: a refused shell costs a button, never
    // a recording.
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    const auto lease = presence_.acquireProgress(TaskbarProgressOwner::RecordingSave);
    EXPECT_TRUE(lease.valid());
    presence_.updateProgress(lease, 0.5);
    EXPECT_DOUBLE_EQ(presence_.progress().fraction(), 0.5);
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdPauseResume)));
}

TEST_F(TaskbarPresenceTest, ARefusedAddButtonsStillLeavesProgressWorking) {
    shell_->fail.add_buttons = true;
    bringUp();

    EXPECT_TRUE(presence_.shellAvailable());
    EXPECT_FALSE(presence_.buttonsRegistered());

    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    // Nothing was registered, so nothing is updated -- but the class carries on.
    EXPECT_EQ(shell_->log.update_buttons, 0);
    const auto lease = presence_.acquireProgress(TaskbarProgressOwner::RecordingSave);
    presence_.updateProgress(lease, 0.4);
    EXPECT_EQ(shell_->last_progress_state, TaskbarProgressState::Normal);
}

TEST_F(TaskbarPresenceTest, ARefusedUpdateButtonsDoesNotStopLaterStates) {
    bringUp();
    shell_->fail.update_buttons = true;
    presence_.setPresence(PresenceFor(UiRecordingState::Recording));
    shell_->fail.update_buttons = false;
    presence_.setPresence(PresenceFor(UiRecordingState::Paused));
    EXPECT_GE(shell_->log.update_buttons, 2);
}

TEST_F(TaskbarPresenceTest, ARefusedProgressCallDoesNotBreakTheLedger) {
    bringUp();
    shell_->fail.progress = true;
    const auto lease = presence_.acquireProgress(TaskbarProgressOwner::EditExport);
    presence_.updateProgress(lease, 0.7);
    EXPECT_DOUBLE_EQ(presence_.progress().fraction(), 0.7);
    presence_.finishProgress(lease);
    EXPECT_FALSE(presence_.progress().held());
}

TEST_F(TaskbarPresenceTest, WithNoPlatformShellAtAllNothingCrashes) {
    TaskbarPresence bare;
    bare.setShellForTest(nullptr);
    bare.setHandle(kHandleA);
    bare.notifyShellReady(kHandleA);
    bare.setPresence(PresenceFor(UiRecordingState::Recording));
    const auto lease = bare.acquireProgress(TaskbarProgressOwner::RecordingSave);
    bare.updateProgress(lease, 0.5);
    bare.finishProgress(lease);
    EXPECT_FALSE(bare.shellAvailable());
    EXPECT_TRUE(bare.handleCommand(Click(kShellButtonIdPauseResume)));
}

// ---------------------------------------------------------------------------
// TrayAdapter -- the notification area's model
// ---------------------------------------------------------------------------
//
// The tray itself is QML on Qt.labs.platform and needs a real notification area
// to exist. What it BINDS to does not, which is the reason the split is there:
// the menu's rows, its labels and the intent behind a click are all assertable
// with no shell involved at all.

namespace {

ShellPresenceState PresenceFor(UiRecordingState state, bool can_start, bool can_stop, bool can_pause, bool can_resume) {
    ShellPresenceInput input;
    input.state = state;
    input.can_start = can_start;
    input.can_stop = can_stop;
    input.can_pause = can_pause;
    input.can_resume = can_resume;
    return ProjectShellPresence(input);
}

} // namespace

TEST(TrayAdapterMenu, IdleOffersOnlyStart) {
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Ready, true, false, false, false), {}, 0);

    EXPECT_TRUE(tray.recordItem().value(QStringLiteral("visible")).toBool());
    EXPECT_TRUE(tray.recordItem().value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(tray.pauseResumeItem().value(QStringLiteral("visible")).toBool());
    EXPECT_FALSE(tray.stopItem().value(QStringLiteral("visible")).toBool());
}

TEST(TrayAdapterMenu, RecordingOffersPauseAndStop) {
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Recording, false, true, true, false), {}, 0);

    EXPECT_FALSE(tray.recordItem().value(QStringLiteral("visible")).toBool());
    EXPECT_TRUE(tray.pauseResumeItem().value(QStringLiteral("visible")).toBool());
    EXPECT_EQ(tray.pauseResumeItem().value(QStringLiteral("text")).toString(), QStringLiteral("Pause recording"));
    EXPECT_TRUE(tray.stopItem().value(QStringLiteral("visible")).toBool());
}

TEST(TrayAdapterMenu, PausedSwapsTheOneEntryToResume) {
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Paused, false, true, false, true), {}, 0);

    EXPECT_EQ(tray.pauseResumeItem().value(QStringLiteral("text")).toString(), QStringLiteral("Resume recording"));
    EXPECT_TRUE(tray.stopItem().value(QStringLiteral("visible")).toBool());
}

TEST(TrayAdapterMenu, ARefusedStartIsGreyedRatherThanGone) {
    TrayAdapter tray;
    // Saving: the session is finishing, so a start exists as an action but is not
    // allowed yet. A vanished entry would read as a bug; a greyed one reads as a
    // reason.
    tray.setPresence(PresenceFor(UiRecordingState::Saving, false, false, false, false), {}, 0);

    EXPECT_TRUE(tray.recordItem().value(QStringLiteral("visible")).toBool());
    EXPECT_FALSE(tray.recordItem().value(QStringLiteral("enabled")).toBool());
}

TEST(TrayAdapterMenu, EveryTransportRowCarriesAGlyph) {
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Recording, false, true, true, false), {}, 0);

    EXPECT_TRUE(tray.pauseResumeItem()
                    .value(QStringLiteral("icon"))
                    .toString()
                    .startsWith(QStringLiteral("image://exosnap-shell/glyph/pause/")));
    EXPECT_TRUE(tray.stopItem()
                    .value(QStringLiteral("icon"))
                    .toString()
                    .startsWith(QStringLiteral("image://exosnap-shell/glyph/stop/")));
}

TEST(TrayAdapterMenu, EveryOtherRowCarriesAGlyphToo) {
    // The four rows that are not transport. A menu where three rows have an icon
    // and four do not reads as three unfinished rows.
    TrayAdapter tray;
    tray.setAppearance(QStringLiteral("dark"), QStringLiteral("aqua"));
    tray.setIconPixelSize(16);

    const QStringList icons{tray.showHideIcon(), tray.outputFolderIcon(), tray.notificationsIcon(), tray.quitIcon()};
    for (const QString& icon : icons) {
        EXPECT_TRUE(icon.startsWith(QStringLiteral("image://exosnap-shell/glyph/"))) << icon.toStdString();
    }
    // And they are four different glyphs, not one drawn four times.
    EXPECT_EQ(QSet<QString>(icons.begin(), icons.end()).size(), icons.size());
}

TEST(TrayAdapterMenu, TheMenuGlyphsFollowTheAccent) {
    TrayAdapter tray;
    tray.setAppearance(QStringLiteral("dark"), QStringLiteral("aqua"));
    tray.setIconPixelSize(16);
    const QString aqua = tray.quitIcon();
    tray.setAppearance(QStringLiteral("dark"), QStringLiteral("magenta"));
    EXPECT_NE(tray.quitIcon(), aqua);
}

TEST(TrayAdapterMenu, OpenOutputFolderAndQuitAreRoutedOutwards) {
    TrayAdapter tray;
    QSignalSpy folder(&tray, &TrayAdapter::openOutputFolderRequested);
    QSignalSpy quit(&tray, &TrayAdapter::quitRequested);

    tray.triggerOpenOutputFolder();
    tray.triggerQuit();
    EXPECT_EQ(folder.count(), 1);
    EXPECT_EQ(quit.count(), 1);
}

TEST(TrayAdapterAction, AnEntryTheStateRefusesRaisesNothing) {
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Recording, false, true, false, false), {}, 0);
    QSignalSpy spy(&tray, &TrayAdapter::shellActionRequested);

    // Pause is offered by the phase but refused by the session's own predicate:
    // an accelerator between the state change and the repaint must not slip
    // through.
    tray.triggerTransport(TrayAdapter::PauseResumeRow);
    EXPECT_EQ(spy.count(), 0);
}

TEST(TrayAdapterAction, ARowRaisesTheIntentTheTableResolved) {
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Paused, false, true, false, true), {}, 0);
    QSignalSpy spy(&tray, &TrayAdapter::shellActionRequested);

    tray.triggerTransport(TrayAdapter::PauseResumeRow);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ShellAction>(), ShellAction::Resume);
}

TEST(TrayAdapterAction, TheStopRowStops) {
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Recording, false, true, true, false), {}, 0);
    QSignalSpy spy(&tray, &TrayAdapter::shellActionRequested);

    tray.triggerTransport(TrayAdapter::StopRow);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ShellAction>(), ShellAction::Stop);
}

TEST(TrayAdapterShowHide, TheLabelDecidesWhichSignalTheEntryRaises) {
    TrayAdapter tray;
    QSignalSpy show(&tray, &TrayAdapter::activateWindowRequested);
    QSignalSpy hide(&tray, &TrayAdapter::hideWindowRequested);

    tray.setWindowVisible(true);
    ASSERT_EQ(tray.showHideText(), QStringLiteral("Hide window"));
    tray.triggerShowHide();
    EXPECT_EQ(hide.count(), 1);
    EXPECT_EQ(show.count(), 0);

    tray.setWindowVisible(false);
    ASSERT_EQ(tray.showHideText(), QStringLiteral("Show window"));
    tray.triggerShowHide();
    EXPECT_EQ(show.count(), 1);
    EXPECT_EQ(hide.count(), 1);
}

TEST(TrayAdapterActivation, ALeftClickAsksForTheWindowAndADoubleClickTogglesRecording) {
    TrayAdapter tray;
    QSignalSpy activate(&tray, &TrayAdapter::activateWindowRequested);
    QSignalSpy toggle(&tray, &TrayAdapter::recordToggleRequested);

    tray.handleActivation(TrayAdapter::TriggerActivation);
    EXPECT_EQ(activate.count(), 1);
    EXPECT_EQ(toggle.count(), 0);

    tray.handleActivation(TrayAdapter::DoubleClickActivation);
    EXPECT_EQ(toggle.count(), 1);

    // A right click opens the menu, which the platform does itself.
    tray.handleActivation(TrayAdapter::ContextActivation);
    EXPECT_EQ(activate.count(), 1);
    EXPECT_EQ(toggle.count(), 1);
}

TEST(TrayAdapterNotifications, TheEntryAppearsWithACountAndClearsOnUse) {
    TrayAdapter tray;
    EXPECT_FALSE(tray.notificationsVisible());

    tray.incrementUnreadCount();
    tray.incrementUnreadCount();
    EXPECT_TRUE(tray.notificationsVisible());
    EXPECT_EQ(tray.notificationsText(), QStringLiteral("Notifications (2)"));

    QSignalSpy activate(&tray, &TrayAdapter::activateWindowRequested);
    tray.triggerNotifications();
    EXPECT_EQ(activate.count(), 1);
    EXPECT_EQ(tray.unreadCount(), 0);
    EXPECT_FALSE(tray.notificationsVisible());
}

TEST(TrayAdapterIcon, TheUrlCarriesTheStateTheSizeAndThePalette) {
    TrayAdapter tray;
    tray.setAppearance(QStringLiteral("light"), QStringLiteral("violet"));
    tray.setIconPixelSize(24);
    tray.setPresence(PresenceFor(UiRecordingState::Paused, false, true, false, true), {}, 0);

    EXPECT_EQ(tray.iconSource(), QStringLiteral("image://exosnap-shell/mark/paused/24/0/shell/light/violet"));
}

TEST(TrayAdapterIcon, TheHeartbeatFrameIsPartOfTheUrl) {
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Recording, false, true, true, false), {}, 0);
    const QString trough = tray.iconSource();
    tray.setPresence(PresenceFor(UiRecordingState::Recording, false, true, true, false), {}, 2);
    EXPECT_NE(tray.iconSource(), trough);
}

TEST(TrayAdapterIcon, AStaticStateIgnoresTheHeartbeatFrame) {
    // Otherwise a tray that was mid-beat when the recording paused would have a
    // different URL for the same paused icon, and would repaint for nothing.
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Paused, false, true, false, true), {}, 0);
    const QString paused = tray.iconSource();
    tray.setPresence(PresenceFor(UiRecordingState::Paused, false, true, false, true), {}, 3);
    EXPECT_EQ(tray.iconSource(), paused);
}

TEST(TrayAdapterIcon, AnAccentChangeRepaintsWithoutARestart) {
    TrayAdapter tray;
    tray.setAppearance(QStringLiteral("dark"), QStringLiteral("aqua"));
    const QString before = tray.iconSource();
    QSignalSpy spy(&tray, &TrayAdapter::appearanceChanged);

    tray.setAppearance(QStringLiteral("dark"), QStringLiteral("magenta"));
    EXPECT_EQ(spy.count(), 1);
    EXPECT_NE(tray.iconSource(), before);
}

TEST(TrayAdapterTooltip, ItNamesTheStateAndCarriesTheClockOnlyWhileRecording) {
    TrayAdapter tray;
    tray.setPresence(PresenceFor(UiRecordingState::Ready, true, false, false, false), QStringLiteral("04:17"), 0);
    EXPECT_EQ(tray.tooltip(), QString::fromUtf8("ExoSnap \xE2\x80\x94 Ready"));

    tray.setPresence(PresenceFor(UiRecordingState::Recording, false, true, true, false), QStringLiteral("04:17"), 0);
    EXPECT_EQ(tray.tooltip(), QString::fromUtf8("ExoSnap \xE2\x80\x94 Recording 04:17"));

    tray.setPresence(PresenceFor(UiRecordingState::Paused, false, true, false, true), QStringLiteral("04:17"), 0);
    EXPECT_EQ(tray.tooltip(), QString::fromUtf8("ExoSnap \xE2\x80\x94 Paused"));
}

TEST(TrayAdapterAvailability, ItIsInactiveUntilTheApplicationSaysThereIsATray) {
    TrayAdapter tray;
    // The QML tray binds its visibility to this, so a session with no
    // notification area instantiates the same object and shows nothing.
    EXPECT_FALSE(tray.active());
    tray.setActive(true);
    EXPECT_TRUE(tray.active());
}
