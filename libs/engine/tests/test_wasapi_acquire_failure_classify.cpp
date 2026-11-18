// Pure classifier for how the WASAPI audio drain must react to an
// IAudioCaptureClient acquire (GetBuffer / GetNextPacketSize) result HRESULT.
// This is the audio mirror of ClassifyOdAcquireFailure: it encodes the
// recording-loss policy so an audio worker cannot loop/wedge through a lost
// endpoint until the fixed join budget detaches it (the a0=TIMEOUT symptom of
// the original recording hang).
//
// This is the SOURCE-LAYER decision — "did this acquire carry data, or did the
// endpoint fail?". The `Fail` return means the old stream is dead (a WASAPI
// capture endpoint invalidated mid-recording cannot be reacquired on the same
// stream). It is pinned here:
//   S_OK / AUDCLNT_S_BUFFER_EMPTY  -> Idle (no data-carrying failure this tick; keep draining)
//   AUDCLNT_E_DEVICE_INVALIDATED   -> Fail (endpoint gone)
//   AUDCLNT_E_SERVICE_NOT_RUNNING  -> Fail (audio service down)
//   any other unexpected HRESULT   -> Fail (fail closed, never loop silently)
//
// A source acquire failure does not by itself end
// the recording. The audio thread now maps a reported acquire failure through
// ClassifyAudioSourceLoss (audio_device_loss_policy.h, pinned in
// test_audio_device_loss_policy.cpp) to DegradeSource — the affected source goes
// to honest silence and reactivates, while video and every other source keep
// running. This classifier still answers only the narrower "is the stream dead"
// question, so its Idle/Fail contract is unchanged.

#include "audio_clock_drift.h"
#include "clock_slaving.h"
#include "wasapi_capture_src.h"
#include "wasapi_device_timing.h"

#include <gtest/gtest.h>

using namespace exosnap::engine;

namespace {

TEST(WasapiAcquireFailure, BufferEmptyIsIdle) {
    // No packet ready this poll tick — not an error, keep draining.
    EXPECT_EQ(ClassifyWasapiAcquireFailure(AUDCLNT_S_BUFFER_EMPTY), WasapiAcquireFailAction::Idle);
}

TEST(WasapiAcquireFailure, BenignSuccessCodeIsIdle) {
    // Defensive: a benign S_OK must not be treated as a fatal loss (would
    // otherwise kill an otherwise-healthy recording on a spurious false return).
    EXPECT_EQ(ClassifyWasapiAcquireFailure(S_OK), WasapiAcquireFailAction::Idle);
}

TEST(WasapiAcquireFailure, DeviceInvalidatedFailsCleanly) {
    // The endpoint was removed / its format changed mid-recording. The stream
    // cannot be reacquired, so end the recording cleanly rather than loop.
    EXPECT_EQ(ClassifyWasapiAcquireFailure(AUDCLNT_E_DEVICE_INVALIDATED), WasapiAcquireFailAction::Fail);
}

TEST(WasapiAcquireFailure, ServiceNotRunningFailsCleanly) {
    // The Windows audio service is not running (stopped / restarting). The
    // capture client is dead for this session; stop cleanly.
    EXPECT_EQ(ClassifyWasapiAcquireFailure(AUDCLNT_E_SERVICE_NOT_RUNNING), WasapiAcquireFailAction::Fail);
}

TEST(WasapiAcquireFailure, UnexpectedHresultFailsCleanly) {
    // The core hardening: any HRESULT we did not explicitly reason about must end
    // the recording cleanly rather than silently loop through a broken endpoint.
    EXPECT_EQ(ClassifyWasapiAcquireFailure(AUDCLNT_E_BUFFER_ERROR), WasapiAcquireFailAction::Fail);
    EXPECT_EQ(ClassifyWasapiAcquireFailure(E_INVALIDARG), WasapiAcquireFailAction::Fail);
    EXPECT_EQ(ClassifyWasapiAcquireFailure(E_FAIL), WasapiAcquireFailAction::Fail);
}

TEST(WasapiDeviceTiming, UncertainOrMissingTimestampIsNotAClockReference) {
    for (const uint32_t flags :
         {0u, uint32_t{AUDCLNT_BUFFERFLAGS_SILENT}, uint32_t{AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY}}) {
        EXPECT_TRUE(HasReliableWasapiTimestamp(flags, 1));
        EXPECT_FALSE(HasReliableWasapiTimestamp(flags, 0));
        EXPECT_FALSE(HasReliableWasapiTimestamp(flags | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR, 1));
    }
}

TEST(WasapiDeviceTiming, UncertainStartupPairCannotPoisonDriftOrEngageSlaving) {
    AudioClockDriftEstimator estimator;
    ClockSlavingController controller;
    constexpr uint64_t origin = 1'000'000'000ULL;
    if (HasReliableWasapiTimestamp(AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR, origin - 70'000'000ULL)) {
        estimator.AddObservation(0, origin - 70'000'000ULL);
        controller.Update(estimator.DriftMs(), 0, origin - 70'000'000ULL);
    }
    for (uint64_t packet = 1; packet <= 256; ++packet) {
        const uint64_t position = packet * 10'000'000ULL;
        ASSERT_TRUE(HasReliableWasapiTimestamp(0, origin + position));
        estimator.AddObservation(position, origin + position);
        controller.Update(estimator.DriftMs(), 0, origin + position);
    }
    EXPECT_DOUBLE_EQ(estimator.DriftMs(), 0.0);
    EXPECT_FALSE(controller.Engaged());
    EXPECT_DOUBLE_EQ(controller.Ppm(), 0.0);
}
} // namespace
