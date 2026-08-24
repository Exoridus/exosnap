// Device-loss policy pins (ADR 0046). Pure, hardware-free: an audio endpoint
// lost mid-recording degrades the affected SOURCE to honest silence and keeps
// the recording running, rather than the old fail-closed behavior that ended
// the whole session. Mirror of test_od_reopen_policy.cpp for the audio path.

#include "audio_device_loss_policy.h"
#include "process_identity.h"

#include <gtest/gtest.h>

using namespace exosnap::engine;
using namespace std::chrono_literals;

namespace {

// --- ClassifyAudioSourceLoss ------------------------------------------------

TEST(AudioDeviceLossPolicy, BufferEmptyKeepsDraining) {
    // A benign "no packet this tick" code is not a device loss.
    EXPECT_EQ(ClassifyAudioSourceLoss(AUDCLNT_S_BUFFER_EMPTY), AudioLossReaction::KeepDraining);
}

TEST(AudioDeviceLossPolicy, DeviceInvalidatedDegradesSource) {
    // The core behavior change: what used to end the recording (Fail/KillSession)
    // now degrades only the affected source (DegradeSource) so the recording and
    // every other source keep running.
    EXPECT_EQ(ClassifyAudioSourceLoss(AUDCLNT_E_DEVICE_INVALIDATED), AudioLossReaction::DegradeSource);
}

TEST(AudioDeviceLossPolicy, ServiceNotRunningDegradesSource) {
    EXPECT_EQ(ClassifyAudioSourceLoss(AUDCLNT_E_SERVICE_NOT_RUNNING), AudioLossReaction::DegradeSource);
}

TEST(AudioDeviceLossPolicy, UnexpectedOrUnattributedFailureDegradesSource) {
    // An unexpected HRESULT, and a failure a source surfaced by message only
    // (hr left at 0 / a generic E_FAIL), both degrade rather than kill — the
    // classifier is only consulted AFTER a failure was already reported.
    EXPECT_EQ(ClassifyAudioSourceLoss(E_FAIL), AudioLossReaction::DegradeSource);
    EXPECT_EQ(ClassifyAudioSourceLoss(0), AudioLossReaction::DegradeSource);
}

// --- DecideAudioDeviceLoss (reactivation cadence) ---------------------------

TEST(AudioDeviceLossPolicy, SuccessfulReactivationGoesLive) {
    const AudioReactivateDecision d = DecideAudioDeviceLoss(true, 0ms, kAudioReactivatePollDelay);
    EXPECT_EQ(d.action, AudioReactivateAction::Reactivated);
}

TEST(AudioDeviceLossPolicy, FailedReactivationRetriesAtPollDelay) {
    const AudioReactivateDecision d = DecideAudioDeviceLoss(false, 3s, kAudioReactivatePollDelay);
    EXPECT_EQ(d.action, AudioReactivateAction::RetryAfter);
    EXPECT_EQ(d.retry_delay, kAudioReactivatePollDelay);
}

TEST(AudioDeviceLossPolicy, ReactivationIsUnbounded) {
    // Unlike the video give-up branch, a degraded audio source never gives up:
    // honest silence does not get worse by waiting, so far past any deadline the
    // decision is still RetryAfter, never a session kill.
    const AudioReactivateDecision d = DecideAudioDeviceLoss(false, 60min, kAudioReactivatePollDelay);
    EXPECT_EQ(d.action, AudioReactivateAction::RetryAfter);
}

TEST(AudioDeviceLossPolicy, PollDelayMatchesWebcamCadence) {
    EXPECT_EQ(kAudioReactivatePollDelay, 500ms);
}

// --- ProcessIdentityMatches (PID-reuse guard for APP/SYS loopback) ----------

TEST(ProcessIdentity, SameInstanceMatches) {
    EXPECT_TRUE(ProcessIdentityMatches(/*captured*/ 0x1234abcdULL, /*current*/ 0x1234abcdULL, /*alive*/ true));
}

TEST(ProcessIdentity, RecycledPidDoesNotMatch) {
    // Same PID, but a different process instance (different creation time) — a
    // stranger. Must fail closed so the loopback is never reacquired onto it.
    EXPECT_FALSE(ProcessIdentityMatches(0x1234abcdULL, 0x9999ffffULL, true));
}

TEST(ProcessIdentity, DeadProcessDoesNotMatch) {
    EXPECT_FALSE(ProcessIdentityMatches(0x1234abcdULL, 0x1234abcdULL, /*alive*/ false));
}

TEST(ProcessIdentity, UnknownCreationTimeFailsClosed) {
    EXPECT_FALSE(ProcessIdentityMatches(0, 0x1234abcdULL, true));
    EXPECT_FALSE(ProcessIdentityMatches(0x1234abcdULL, 0, true));
}

} // namespace
