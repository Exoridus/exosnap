#include "ShellPresenceAdapter.h"
#include "TaskbarPresence.h"
#include "models/RecordingPulse.h"
#include "models/ShellPresence.h"
#include "models/TaskbarProgressLease.h"
#include "viewmodels/RecordViewModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using exosnap::kRecordingPulseFrameCount;
using exosnap::kShellButtonIdPauseResume;
using exosnap::kShellButtonIdRecord;
using exosnap::kShellButtonIdStop;
using exosnap::ProjectShellPresence;
using exosnap::RecordingPulseIntensity;
using exosnap::ShellAction;
using exosnap::ShellIconState;
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
        int overlay = 0;
        int progress_state = 0;
        int progress_value = 0;
    };

    struct Failures {
        bool initialize = false;
        bool add_buttons = false;
        bool update_buttons = false;
        bool overlay = false;
        bool progress = false;
    };

    Log log;
    Failures fail;
    QVector<ThumbButtonSpec> last_buttons;
    ShellIconState last_overlay = ShellIconState::Idle;
    int last_pulse_level = -1;
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
    qint32 setOverlayIcon(void* hwnd, ShellIconState state, int pulse_level) override {
        ++log.overlay;
        handles.push_back(hwnd);
        last_overlay = state;
        last_pulse_level = pulse_level;
        return fail.overlay ? kFail : kOk;
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

TEST(ShellPresenceAdapterPulse, AStoppedPulseReportsTheTroughRatherThanItsLastFrame) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.advancePulseForTest();
    adapter.advancePulseForTest();
    ASSERT_GT(adapter.pulseIntensity(), 0.0);

    adapter.setRecordingState(UiRecordingState::Paused, false, true, false, true, false);
    EXPECT_DOUBLE_EQ(adapter.pulseIntensity(), 0.0);
    EXPECT_EQ(adapter.taskbarPulseLevel(), 0);
}

TEST(ShellPresenceAdapterPulse, EveryRecordingStartsAtTheTrough) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    adapter.advancePulseForTest();
    ASSERT_NE(adapter.pulseFrame(), 0);

    adapter.setRecordingState(UiRecordingState::Ready, true, false, false, false, false);
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    EXPECT_EQ(adapter.pulseFrame(), 0);
}

TEST(ShellPresenceAdapterPulse, ThePhaseWrapsThroughTheWholeBeat) {
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    for (int i = 0; i < kRecordingPulseFrameCount; ++i)
        adapter.advancePulseForTest();
    EXPECT_EQ(adapter.pulseFrame(), 0);
    EXPECT_DOUBLE_EQ(adapter.pulseIntensity(), RecordingPulseIntensity(0));
}

TEST(ShellPresenceAdapterPulse, TheTimerActuallyTicksOnItsOwn) {
    // The seam above drives the handler directly; this is the counter-check that
    // there is a timer behind it at all.
    EnsureApplication();
    ShellPresenceAdapter adapter;
    adapter.setRecordingState(UiRecordingState::Recording, false, true, true, false, false);
    ASSERT_EQ(adapter.pulseFrame(), 0);
    EXPECT_TRUE(SpinUntil([&adapter]() { return adapter.pulseFrame() != 0; }));
}

// -- ShellPresenceAdapter: saved dwell ---------------------------------------

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
    EXPECT_EQ(adapter.presence().icon_state, ShellIconState::Idle);
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
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);

    EXPECT_FALSE(presence_.ready());
    EXPECT_EQ(shell_->log.add_buttons, 0);
    EXPECT_EQ(shell_->log.update_buttons, 0);
    EXPECT_EQ(shell_->log.overlay, 0);
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
    EXPECT_EQ(shell_->last_buttons.size(), 3);
}

TEST_F(TaskbarPresenceTest, ARepeatedAnnouncementReArmsWithoutDuplicatingTheSet) {
    // Explorer broadcasts TaskbarButtonCreated again after it restarts, and the
    // button it announces then is a NEW one with none of this window's
    // registrations. Ignoring the repeat would cost the user the thumbnail
    // transport for the rest of the session -- so it re-arms, and the set it
    // registers is still exactly the same three slots.
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    ASSERT_EQ(shell_->log.add_buttons, 1);

    presence_.notifyShellReady(kHandleA);
    EXPECT_EQ(shell_->log.add_buttons, 2);
    EXPECT_EQ(shell_->last_buttons.size(), 3);
    EXPECT_TRUE(presence_.buttonsRegistered());
    // And the desired state is on the new button, not left over on the old one.
    EXPECT_EQ(shell_->last_overlay, ShellIconState::Recording);
    EXPECT_EQ(shell_->last_pulse_level, 1);
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
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    ASSERT_EQ(shell_->log.overlay, 0);

    presence_.notifyShellReady(kHandleA);
    EXPECT_EQ(shell_->last_overlay, ShellIconState::Recording);
    EXPECT_EQ(shell_->last_pulse_level, 1);
    ASSERT_EQ(shell_->last_buttons.size(), 3);
    EXPECT_FALSE(shell_->last_buttons[0].visible); // Record
    EXPECT_TRUE(shell_->last_buttons[1].visible);  // Pause
    EXPECT_TRUE(shell_->last_buttons[2].visible);  // Stop
}

TEST_F(TaskbarPresenceTest, AHandleIdentityChangeDropsReadinessAndTheAppliedState) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    ASSERT_TRUE(presence_.ready());

    presence_.setHandle(kHandleB);
    EXPECT_FALSE(presence_.ready());
    EXPECT_FALSE(presence_.buttonsRegistered());

    // A new taskbar button has been told nothing, so nothing may be sent to it.
    const int before = shell_->log.overlay + shell_->log.update_buttons + shell_->log.add_buttons;
    presence_.setPresence(PresenceFor(UiRecordingState::Paused), 0);
    EXPECT_EQ(shell_->log.overlay + shell_->log.update_buttons + shell_->log.add_buttons, before);
}

TEST_F(TaskbarPresenceTest, TheNewHandleNeedsItsOwnReadinessAndThenGetsTheFullState) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    presence_.setHandle(kHandleB);
    presence_.setPresence(PresenceFor(UiRecordingState::Paused), 0);

    presence_.notifyShellReady(kHandleB);
    EXPECT_TRUE(presence_.ready());
    EXPECT_TRUE(presence_.buttonsRegistered());
    EXPECT_EQ(shell_->last_overlay, ShellIconState::Paused);
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
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    const int overlay = shell_->log.overlay;
    const int updates = shell_->log.update_buttons;

    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    EXPECT_EQ(shell_->log.overlay, overlay);
    EXPECT_EQ(shell_->log.update_buttons, updates);
}

TEST_F(TaskbarPresenceTest, ANewPulseLevelRedrawsOnlyTheBadge) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 0);
    const int updates = shell_->log.update_buttons;
    const int overlay = shell_->log.overlay;

    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    EXPECT_EQ(shell_->log.overlay, overlay + 1);
    // The transport did not change, so the strip is not resent.
    EXPECT_EQ(shell_->log.update_buttons, updates);
}

TEST_F(TaskbarPresenceTest, IdleClearsTheBadge) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    presence_.setPresence(PresenceFor(UiRecordingState::Ready), 0);
    EXPECT_EQ(shell_->last_overlay, ShellIconState::Idle);
}

// -- TaskbarPresence: buttons -----------------------------------------------

TEST_F(TaskbarPresenceTest, RecordButtonStartsFromIdle) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Ready), 0);
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdRecord)));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ShellAction>(), ShellAction::Start);
}

TEST_F(TaskbarPresenceTest, TheSharedButtonPausesThenResumes) {
    bringUp();
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);

    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 0);
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdPauseResume)));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ShellAction>(), ShellAction::Pause);

    presence_.setPresence(PresenceFor(UiRecordingState::Paused), 0);
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdPauseResume)));
    ASSERT_EQ(spy.count(), 2);
    EXPECT_EQ(spy.at(1).at(0).value<ShellAction>(), ShellAction::Resume);
}

TEST_F(TaskbarPresenceTest, StopButtonStops) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 0);
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdStop)));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ShellAction>(), ShellAction::Stop);
}

TEST_F(TaskbarPresenceTest, AnUnknownCommandIsNotOursAndAsksForNothing) {
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 0);
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);
    EXPECT_FALSE(presence_.handleCommand(Click(0x1234)));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(TaskbarPresenceTest, AWmCommandThatIsNotAThumbnailClickIsNotOurs) {
    // The filter is process-wide, and WM_COMMAND carries menu and accelerator
    // notifications too. Only THBN_CLICKED in the high word makes it ours.
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 0);
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);
    EXPECT_FALSE(presence_.handleCommand(static_cast<quint64>(kShellButtonIdStop)));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(TaskbarPresenceTest, ACommandFromAStaleStripIsConsumedAndDoesNothing) {
    // Explorer delivers the click against the strip it last painted, which can
    // be a state the session has already left.
    bringUp();
    presence_.setPresence(PresenceFor(UiRecordingState::Saving), 0);
    QSignalSpy spy(&presence_, &TaskbarPresence::actionRequested);

    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdStop)));
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdPauseResume)));
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdRecord)));
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(TaskbarPresenceTest, TheStripAClickIsResolvedAgainstIsTheStripThatWasSent) {
    bringUp();
    const ShellPresenceState recording = PresenceFor(UiRecordingState::Recording);
    presence_.setPresence(recording, 0);
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
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    const auto lease = presence_.acquireProgress(TaskbarProgressOwner::RecordingSave);
    EXPECT_TRUE(lease.valid());
    presence_.updateProgress(lease, 0.5);
    EXPECT_DOUBLE_EQ(presence_.progress().fraction(), 0.5);
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdPauseResume)));
}

TEST_F(TaskbarPresenceTest, ARefusedAddButtonsStillLeavesTheBadgeAndProgressWorking) {
    shell_->fail.add_buttons = true;
    bringUp();

    EXPECT_TRUE(presence_.shellAvailable());
    EXPECT_FALSE(presence_.buttonsRegistered());

    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    EXPECT_EQ(shell_->last_overlay, ShellIconState::Recording);
    // Nothing was registered, so nothing is updated -- but the class carries on.
    EXPECT_EQ(shell_->log.update_buttons, 0);
}

TEST_F(TaskbarPresenceTest, ARefusedUpdateButtonsDoesNotStopLaterStates) {
    bringUp();
    shell_->fail.update_buttons = true;
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 0);
    shell_->fail.update_buttons = false;
    presence_.setPresence(PresenceFor(UiRecordingState::Paused), 0);
    EXPECT_EQ(shell_->last_overlay, ShellIconState::Paused);
    EXPECT_GE(shell_->log.update_buttons, 2);
}

TEST_F(TaskbarPresenceTest, ARefusedOverlayIconDoesNotStopTheButtons) {
    bringUp();
    shell_->fail.overlay = true;
    presence_.setPresence(PresenceFor(UiRecordingState::Recording), 0);
    EXPECT_GE(shell_->log.update_buttons, 1);
    EXPECT_TRUE(presence_.handleCommand(Click(kShellButtonIdStop)));
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
    bare.setPresence(PresenceFor(UiRecordingState::Recording), 1);
    const auto lease = bare.acquireProgress(TaskbarProgressOwner::RecordingSave);
    bare.updateProgress(lease, 0.5);
    bare.finishProgress(lease);
    EXPECT_FALSE(bare.shellAvailable());
    EXPECT_TRUE(bare.handleCommand(Click(kShellButtonIdPauseResume)));
}
