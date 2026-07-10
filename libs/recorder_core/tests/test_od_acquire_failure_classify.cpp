// Pure classifier for how the DXGI Output Duplication drain must react to a
// TryAcquireFrame failure HRESULT. This encodes the recording-loss policy that
// the video-thread drain previously got wrong: only DXGI_ERROR_ACCESS_LOST was
// handled, so DXGI_ERROR_DEVICE_REMOVED (and every other HRESULT) fell through a
// bare `break` — no source-loss, no failure — and the worker looped through a
// dead GPU until the fixed 120 s join budget detached it (unfinalised .mkv).
//
// The decision is D3D-free and pinned here:
//   WAIT_TIMEOUT  -> Idle    (no frame this poll; normal, keep looping)
//   ACCESS_LOST   -> Recover (duplication handle stale, device alive: recreate)
//   DEVICE_REMOVED/HUNG/RESET or any unexpected HRESULT -> Fail (stop cleanly)

#include <recorder_core/dxgi_od_capture_src.h>

#include <gtest/gtest.h>

using namespace recorder_core;

namespace {

TEST(OdAcquireFailure, WaitTimeoutIsIdle) {
    // No frame available this poll tick — not an error, keep draining.
    EXPECT_EQ(ClassifyOdAcquireFailure(DXGI_ERROR_WAIT_TIMEOUT), OdAcquireFailAction::Idle);
}

TEST(OdAcquireFailure, BenignSuccessCodeIsIdle) {
    // Defensive: a false return that left out_hr at S_OK must not be treated as a
    // fatal error (would otherwise kill an otherwise-healthy recording).
    EXPECT_EQ(ClassifyOdAcquireFailure(S_OK), OdAcquireFailAction::Idle);
}

TEST(OdAcquireFailure, AccessLostIsRecoverable) {
    // Duplication invalidated by a mode/topology change (fullscreen, refresh/HDR
    // switch, monitor re-negotiation). The D3D device is still alive, so the
    // duplication can be recreated and the same encode session continued.
    EXPECT_EQ(ClassifyOdAcquireFailure(DXGI_ERROR_ACCESS_LOST), OdAcquireFailAction::Recover);
}

TEST(OdAcquireFailure, DeviceRemovedFailsCleanly) {
    // The regression that hung recordings: the device itself is gone. Must end the
    // recording cleanly (source-loss -> EOS -> finalise), never loop or wedge.
    EXPECT_EQ(ClassifyOdAcquireFailure(DXGI_ERROR_DEVICE_REMOVED), OdAcquireFailAction::Fail);
}

TEST(OdAcquireFailure, DeviceHungAndResetFailCleanly) {
    EXPECT_EQ(ClassifyOdAcquireFailure(DXGI_ERROR_DEVICE_HUNG), OdAcquireFailAction::Fail);
    EXPECT_EQ(ClassifyOdAcquireFailure(DXGI_ERROR_DEVICE_RESET), OdAcquireFailAction::Fail);
}

TEST(OdAcquireFailure, UnexpectedHresultFailsCleanly) {
    // The core hardening: any HRESULT we did not explicitly reason about must end
    // the recording cleanly rather than silently loop through a broken source.
    EXPECT_EQ(ClassifyOdAcquireFailure(E_INVALIDARG), OdAcquireFailAction::Fail);
    EXPECT_EQ(ClassifyOdAcquireFailure(E_FAIL), OdAcquireFailAction::Fail);
}

} // namespace
