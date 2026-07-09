// Pure policy for the webcam capture loop's reaction to one ReadSample() result.
// ClassifyWebcamReadResult inspects only the returned HRESULT and reader flags, so
// the loss-detection / reconnect policy is unit-pinned (no live device needed),
// mirroring recorder_core's ClassifyOdAcquireFailure. A dead reader (failed HRESULT,
// MF_SOURCE_READERF_ERROR, or MF_SOURCE_READERF_ENDOFSTREAM) maps to Reconnect so the
// capture thread reopens the device while TryGetFrame keeps serving the last frame.

#include "services/WebcamService.h"

// MF header order matches WebcamService.cpp: mfidl.h declares the interfaces
// (IMFMediaSource / IMFSourceReader) that mfreadwrite.h references.
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <gtest/gtest.h>

using namespace exosnap;

namespace {

TEST(WebcamReadPolicy, HealthyReadWithSampleDelivers) {
    EXPECT_EQ(ClassifyWebcamReadResult(S_OK, 0, /*has_sample=*/true), WebcamReadAction::Deliver);
}

TEST(WebcamReadPolicy, HealthyReadWithoutSampleSkips) {
    // A read that returns S_OK but no sample (spurious wake) keeps the last frame.
    EXPECT_EQ(ClassifyWebcamReadResult(S_OK, 0, /*has_sample=*/false), WebcamReadAction::Skip);
}

TEST(WebcamReadPolicy, StreamTickWithoutSampleSkips) {
    // A streaming tick (gap marker) carries no sample and is not a loss.
    EXPECT_EQ(ClassifyWebcamReadResult(S_OK, MF_SOURCE_READERF_STREAMTICK, /*has_sample=*/false),
              WebcamReadAction::Skip);
}

TEST(WebcamReadPolicy, FailedHresultReconnects) {
    EXPECT_EQ(ClassifyWebcamReadResult(E_FAIL, 0, /*has_sample=*/false), WebcamReadAction::Reconnect);
}

TEST(WebcamReadPolicy, DeviceInvalidatedHresultReconnects) {
    // A representative device-loss HRESULT surfaced by ReadSample.
    EXPECT_EQ(ClassifyWebcamReadResult(MF_E_HW_MFT_FAILED_START_STREAMING, 0, /*has_sample=*/false),
              WebcamReadAction::Reconnect);
}

TEST(WebcamReadPolicy, ReaderErrorFlagReconnects) {
    EXPECT_EQ(ClassifyWebcamReadResult(S_OK, MF_SOURCE_READERF_ERROR, /*has_sample=*/false),
              WebcamReadAction::Reconnect);
}

TEST(WebcamReadPolicy, EndOfStreamReconnects) {
    // A live capture stream ending means the device went away — reopen it.
    EXPECT_EQ(ClassifyWebcamReadResult(S_OK, MF_SOURCE_READERF_ENDOFSTREAM, /*has_sample=*/false),
              WebcamReadAction::Reconnect);
}

TEST(WebcamReadPolicy, ErrorFlagWinsOverAStaleSample) {
    // Even if a sample pointer is present, a fatal reader error must reconnect
    // rather than deliver a frame from a dying reader.
    EXPECT_EQ(ClassifyWebcamReadResult(S_OK, MF_SOURCE_READERF_ERROR, /*has_sample=*/true),
              WebcamReadAction::Reconnect);
}

// ---------------------------------------------------------------------------
// ResolveWebcamDeviceId (BUG 2: no silent devices[0] fallback)
// ---------------------------------------------------------------------------

TEST(ResolveWebcamDeviceIdTest, EmptyConfiguredWithDevicesReturnsFirst) {
    const std::vector<WebcamDeviceInfo> devices = {{"A", "Cam A"}, {"B", "Cam B"}};
    EXPECT_EQ(ResolveWebcamDeviceId("", devices), "A");
}

TEST(ResolveWebcamDeviceIdTest, ConfiguredPresentDeviceIsKept) {
    const std::vector<WebcamDeviceInfo> devices = {{"A", "Cam A"}, {"B", "Cam B"}};
    EXPECT_EQ(ResolveWebcamDeviceId("B", devices), "B");
}

TEST(ResolveWebcamDeviceIdTest, ConfiguredAbsentDeviceIsKeptForReconnect) {
    // An explicit choice is kept even when momentarily absent, so it reconnects
    // when plugged back in — it must NOT silently fall back to another device.
    const std::vector<WebcamDeviceInfo> devices = {{"A", "Cam A"}, {"B", "Cam B"}};
    EXPECT_EQ(ResolveWebcamDeviceId("X", devices), "X");
}

TEST(ResolveWebcamDeviceIdTest, EmptyConfiguredWithNoDevicesReturnsEmpty) {
    EXPECT_EQ(ResolveWebcamDeviceId("", {}), "");
}

// ---------------------------------------------------------------------------
// ShouldDeliverWebcamSample (BUG 3: timestamp monotonicity gate)
// ---------------------------------------------------------------------------

TEST(ShouldDeliverWebcamSampleTest, FirstFrameAlwaysPasses) {
    // last_delivered_100ns < 0 means "no frame delivered yet".
    EXPECT_TRUE(ShouldDeliverWebcamSample(-1, 12345));
}

TEST(ShouldDeliverWebcamSampleTest, NewerSamplePasses) {
    EXPECT_TRUE(ShouldDeliverWebcamSample(1000, 2000));
}

TEST(ShouldDeliverWebcamSampleTest, EqualSampleIsDropped) {
    EXPECT_FALSE(ShouldDeliverWebcamSample(1000, 1000));
}

TEST(ShouldDeliverWebcamSampleTest, OlderSampleIsDropped) {
    // This is the "snap back several frames" glitch: a reopened reader replays
    // buffered older frames — they must be dropped rather than delivered.
    EXPECT_FALSE(ShouldDeliverWebcamSample(1000, 500));
}

TEST(ShouldDeliverWebcamSampleTest, NonPositiveSampleTimestampPasses) {
    // No basis to reject when the sample carries no usable timestamp.
    EXPECT_TRUE(ShouldDeliverWebcamSample(1000, 0));
    EXPECT_TRUE(ShouldDeliverWebcamSample(1000, -5));
}

} // namespace
