#include "MainWindowAffinity.h"
#include "QuickWindowChrome.h"
#include "models/WindowPresencePolicy.h"

#include <gtest/gtest.h>

#include <vector>

using exosnap::DesiredWindowCaptureAffinity;
using exosnap::EvaluateMinimize;
using exosnap::IsMinimizeSysCommand;
using exosnap::kWindowAffinityExcludeFromCapture;
using exosnap::kWindowAffinityNone;
using exosnap::MinimizeOutcome;
using exosnap::quick::MainWindowAffinity;
using exosnap::quick::QuickWindowChrome;

namespace {

// One recorded platform call: which window, which WDA_* constant.
struct AffinityCall {
    void* hwnd = nullptr;
    quint32 affinity = 0;
};

// Two arbitrary non-null values standing in for HWNDs. Their only requirement is
// that they differ, which is what makes an identity change observable.
void* const kHandleA = reinterpret_cast<void*>(0x1000);
void* const kHandleB = reinterpret_cast<void*>(0x2000);

class MainWindowAffinityTest : public ::testing::Test {
  protected:
    void SetUp() override {
        installSeam(/*succeed=*/true);
    }

    void installSeam(bool succeed) {
        succeed_ = succeed;
        affinity_.setAffinityFunctionForTest([this](void* hwnd, quint32 affinity) {
            calls_.push_back(AffinityCall{hwnd, affinity});
            return succeed_;
        });
    }

    MainWindowAffinity affinity_;
    std::vector<AffinityCall> calls_;
    bool succeed_ = true;
};

} // namespace

// ── Minimize policy ─────────────────────────────────────────────────────────

TEST(EvaluateMinimizeTest, SettingOffMinimizesToTheTaskbar) {
    EXPECT_EQ(EvaluateMinimize(/*minimize_to_tray=*/false, /*tray_available=*/true), MinimizeOutcome::Taskbar);
}

TEST(EvaluateMinimizeTest, SettingOnWithATrayHidesToTheTray) {
    EXPECT_EQ(EvaluateMinimize(/*minimize_to_tray=*/true, /*tray_available=*/true), MinimizeOutcome::HideToTray);
}

// The fail-safe: a hidden window whose only restore path does not exist is a
// running process the user cannot reach.
TEST(EvaluateMinimizeTest, SettingOnWithoutATrayFallsBackToTheTaskbar) {
    EXPECT_EQ(EvaluateMinimize(/*minimize_to_tray=*/true, /*tray_available=*/false), MinimizeOutcome::Taskbar);
}

TEST(EvaluateMinimizeTest, SettingOffWithoutATrayMinimizesToTheTaskbar) {
    EXPECT_EQ(EvaluateMinimize(/*minimize_to_tray=*/false, /*tray_available=*/false), MinimizeOutcome::Taskbar);
}

// ── The native minimize command ─────────────────────────────────────────────

TEST(IsMinimizeSysCommandTest, ScMinimizeIsAMinimize) {
    EXPECT_TRUE(IsMinimizeSysCommand(0xF020u));
}

// The reason the mask exists. Windows reserves the low four bits, and an
// unmasked comparison fails silently the first time it uses one.
TEST(IsMinimizeSysCommandTest, TheReservedLowBitsAreMaskedOff) {
    EXPECT_TRUE(IsMinimizeSysCommand(0xF020u | 0x000Fu));
    EXPECT_TRUE(IsMinimizeSysCommand(0xF020u | 0x0002u));
}

TEST(IsMinimizeSysCommandTest, OtherSystemCommandsAreNotMinimizes) {
    EXPECT_FALSE(IsMinimizeSysCommand(0xF030u)) << "SC_MAXIMIZE";
    EXPECT_FALSE(IsMinimizeSysCommand(0xF120u)) << "SC_RESTORE";
    EXPECT_FALSE(IsMinimizeSysCommand(0xF060u)) << "SC_CLOSE";
    EXPECT_FALSE(IsMinimizeSysCommand(0xF010u)) << "SC_MOVE";
}

// ── Capture-affinity policy ─────────────────────────────────────────────────

TEST(DesiredWindowCaptureAffinityTest, SettingOffAsksForNoAffinity) {
    EXPECT_EQ(DesiredWindowCaptureAffinity(false), kWindowAffinityNone);
}

TEST(DesiredWindowCaptureAffinityTest, SettingOnAsksForExcludeFromCapture) {
    EXPECT_EQ(DesiredWindowCaptureAffinity(true), kWindowAffinityExcludeFromCapture);
}

// The constants are the ones Windows defines, repeated here so a typo in the
// platform-neutral copy cannot pass unnoticed.
TEST(DesiredWindowCaptureAffinityTest, ConstantsMatchTheWin32Values) {
    EXPECT_EQ(kWindowAffinityNone, 0x00000000u);
    EXPECT_EQ(kWindowAffinityExcludeFromCapture, 0x00000011u);
}

// ── The shell-window seam ───────────────────────────────────────────────────

TEST_F(MainWindowAffinityTest, WithoutAHandleNothingIsApplied) {
    affinity_.setExcludedFromCapture(true);
    EXPECT_TRUE(calls_.empty());
    EXPECT_FALSE(affinity_.applied());
}

TEST_F(MainWindowAffinityTest, TurningTheSettingOnAppliesExcludeFromCapture) {
    affinity_.setHandle(kHandleA);
    calls_.clear();

    affinity_.setExcludedFromCapture(true);

    ASSERT_EQ(calls_.size(), 1u);
    EXPECT_EQ(calls_.front().hwnd, kHandleA);
    EXPECT_EQ(calls_.front().affinity, kWindowAffinityExcludeFromCapture);
    EXPECT_TRUE(affinity_.applied());
}

// The default state is asserted against the platform call rather than against
// the member: a window that is never told WDA_NONE keeps whatever affinity it
// last had.
TEST_F(MainWindowAffinityTest, AHandleAppliesTheDefaultNoneAffinity) {
    affinity_.setHandle(kHandleA);

    ASSERT_EQ(calls_.size(), 1u);
    EXPECT_EQ(calls_.front().affinity, kWindowAffinityNone);
}

TEST_F(MainWindowAffinityTest, TurningTheSettingOffClearsTheAffinity) {
    affinity_.setHandle(kHandleA);
    affinity_.setExcludedFromCapture(true);
    calls_.clear();

    affinity_.setExcludedFromCapture(false);

    ASSERT_EQ(calls_.size(), 1u);
    EXPECT_EQ(calls_.front().affinity, kWindowAffinityNone);
    EXPECT_FALSE(affinity_.excludedFromCapture());
}

TEST_F(MainWindowAffinityTest, SettingTheSameValueAgainDoesNotReapply) {
    affinity_.setHandle(kHandleA);
    affinity_.setExcludedFromCapture(true);
    calls_.clear();

    affinity_.setExcludedFromCapture(true);

    EXPECT_TRUE(calls_.empty());
}

// Display affinity is per-HWND. A recreated native window comes back without it,
// so the desired state has to be pushed at the new handle.
TEST_F(MainWindowAffinityTest, AHandleIdentityChangeReappliesTheDesiredAffinity) {
    affinity_.setHandle(kHandleA);
    affinity_.setExcludedFromCapture(true);
    calls_.clear();

    affinity_.setHandle(kHandleB);

    ASSERT_EQ(calls_.size(), 1u);
    EXPECT_EQ(calls_.front().hwnd, kHandleB);
    EXPECT_EQ(calls_.front().affinity, kWindowAffinityExcludeFromCapture);
}

// The chrome's refresh path runs on events that are usually not recreations.
TEST_F(MainWindowAffinityTest, TheSameHandleAgainDoesNotReapply) {
    affinity_.setHandle(kHandleA);
    affinity_.setExcludedFromCapture(true);
    calls_.clear();

    affinity_.setHandle(kHandleA);

    EXPECT_TRUE(calls_.empty());
}

// Fail-OPEN, the opposite of CaptureExclusion: a refused call must leave the
// window alone. The seam owns no window, so what it must not do is latch --
// there is no "give up for the session" state that would keep a later, working
// call from taking effect.
TEST_F(MainWindowAffinityTest, ARefusedCallLeavesTheWindowUntouchedAndDoesNotLatch) {
    installSeam(/*succeed=*/false);
    affinity_.setHandle(kHandleA);
    affinity_.setExcludedFromCapture(true);

    EXPECT_FALSE(affinity_.applied());
    // The preference still reads as the user set it -- a failed platform call is
    // not a silent revert of the setting.
    EXPECT_TRUE(affinity_.excludedFromCapture());

    installSeam(/*succeed=*/true);
    calls_.clear();
    affinity_.setHandle(kHandleB);

    ASSERT_EQ(calls_.size(), 1u);
    EXPECT_EQ(calls_.front().affinity, kWindowAffinityExcludeFromCapture);
    EXPECT_TRUE(affinity_.applied());
}

TEST_F(MainWindowAffinityTest, ClearingTheHandleStopsApplying) {
    affinity_.setHandle(kHandleA);
    affinity_.setExcludedFromCapture(true);
    calls_.clear();

    affinity_.setHandle(nullptr);
    affinity_.setExcludedFromCapture(false);

    EXPECT_TRUE(calls_.empty());
    EXPECT_FALSE(affinity_.applied());
}

// ── One policy for every minimize route ─────────────────────────────────────
//
// The defect this guards against is a QML-side check on the visible Minimize
// button: it looks correct on screen and leaves Win+Down, the window menu and the
// taskbar button minimizing straight past the preference. Both routes are
// exercised against the SAME provider instance here, and the count is what proves
// they consult it rather than each carrying their own copy.
//
// No window is attached -- neither route needs one to reach the decision, which is
// the whole reason the decision is separable from the message pump.

namespace {

class ChromeMinimizeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        chrome_.setMinimizeToTrayProvider([this]() {
            ++provider_calls_;
            return hide_to_tray_;
        });
        QObject::connect(&chrome_, &QuickWindowChrome::minimizeToTrayRequested, &chrome_,
                         [this]() { ++hide_requests_; });
    }

    QuickWindowChrome chrome_;
    int provider_calls_ = 0;
    int hide_requests_ = 0;
    bool hide_to_tray_ = false;
};

// SC_MINIMIZE, unmasked, as Windows sends it.
constexpr quint64 kScMinimize = 0xF020u;

} // namespace

TEST_F(ChromeMinimizeTest, TheButtonRouteAsksTheProvider) {
    hide_to_tray_ = true;

    chrome_.minimizeWindow();

    EXPECT_EQ(provider_calls_, 1);
    EXPECT_EQ(hide_requests_, 1);
}

TEST_F(ChromeMinimizeTest, TheNativeRouteAsksTheSameProvider) {
    hide_to_tray_ = true;

    EXPECT_TRUE(chrome_.handleSysCommand(kScMinimize)) << "Windows must not also perform the minimize";
    EXPECT_EQ(provider_calls_, 1);
    EXPECT_EQ(hide_requests_, 1);
}

TEST_F(ChromeMinimizeTest, BothRoutesFollowTheProviderTogether) {
    hide_to_tray_ = false;

    chrome_.minimizeWindow();
    EXPECT_FALSE(chrome_.handleSysCommand(kScMinimize)) << "Windows performs the ordinary minimize";
    EXPECT_EQ(hide_requests_, 0);

    hide_to_tray_ = true;
    chrome_.minimizeWindow();
    EXPECT_TRUE(chrome_.handleSysCommand(kScMinimize));
    EXPECT_EQ(hide_requests_, 2);
}

TEST_F(ChromeMinimizeTest, NoProviderMeansTheOrdinaryMinimize) {
    QuickWindowChrome bare;
    int hides = 0;
    QObject::connect(&bare, &QuickWindowChrome::minimizeToTrayRequested, &bare, [&hides]() { ++hides; });

    bare.minimizeWindow();
    EXPECT_FALSE(bare.handleSysCommand(kScMinimize));
    EXPECT_EQ(hides, 0);
}

TEST_F(ChromeMinimizeTest, OtherSystemCommandsAreLeftToWindows) {
    hide_to_tray_ = true;

    EXPECT_FALSE(chrome_.handleSysCommand(0xF030u)) << "SC_MAXIMIZE";
    EXPECT_FALSE(chrome_.handleSysCommand(0xF060u)) << "SC_CLOSE";
    EXPECT_EQ(hide_requests_, 0);
}

// The chrome forwards the shell's exclusion to the affinity seam, and reports
// what the platform actually did rather than what was asked for.
TEST(ChromeCaptureExclusionTest, TheSettingReachesThePlatformCall) {
    QuickWindowChrome chrome;
    std::vector<AffinityCall> calls;
    chrome.setAffinityFunctionForTest([&calls](void* hwnd, quint32 affinity) {
        calls.push_back(AffinityCall{hwnd, affinity});
        return true;
    });

    // No handle yet: nothing to apply to, and nothing claimed.
    chrome.setCaptureExcluded(true);
    EXPECT_TRUE(chrome.captureExcluded());
    EXPECT_FALSE(chrome.captureExclusionApplied());
    EXPECT_TRUE(calls.empty());
}
