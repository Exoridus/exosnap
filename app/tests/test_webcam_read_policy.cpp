// Pure policy for the webcam capture loop's reaction to one ReadSample() result.
// ClassifyWebcamReadResult inspects only the returned HRESULT and reader flags, so
// the loss-detection / reconnect policy is unit-pinned (no live device needed),
// mirroring the engine's ClassifyOdAcquireFailure. A dead reader (failed HRESULT,
// MF_SOURCE_READERF_ERROR, or MF_SOURCE_READERF_ENDOFSTREAM) maps to Reconnect so the
// capture thread reopens the device while TryGetFrame keeps serving the last frame.

#include "services/WebcamReadCallback.h"
#include "services/WebcamService.h"

// MF header order matches WebcamService.cpp: mfidl.h declares the interfaces
// (IMFMediaSource / IMFSourceReader) that mfreadwrite.h references.
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <thread>

using namespace exosnap;

namespace {

TEST(WebcamReadPolicy, StopWakesAnOutstandingAsyncReadWithoutADeviceCallback) {
    auto callback = Microsoft::WRL::Make<WebcamReadCallback>();
    std::promise<void> entered;
    auto started = entered.get_future();
    std::promise<bool> completed;
    auto result = completed.get_future();
    std::jthread worker([&](std::stop_token stop) {
        entered.set_value();
        WebcamReadCallback::Sample sample;
        completed.set_value(callback->Wait(stop, sample));
    });
    started.wait();
    worker.request_stop();
    const auto stopped = result.wait_for(std::chrono::milliseconds(250));
    callback->OnReadSample(S_OK, 0, 0, 42, nullptr);
    worker.join();
    EXPECT_EQ(stopped, std::future_status::ready);
    EXPECT_FALSE(result.get());
}

TEST(WebcamReadPolicy, CallbackOwnsOneResultAndIgnoresDeliveryAfterClose) {
    auto callback = Microsoft::WRL::Make<WebcamReadCallback>();
    Microsoft::WRL::ComPtr<IMFSample> original;
    ASSERT_EQ(MFCreateSample(&original), S_OK);
    ASSERT_EQ(original->SetSampleTime(123), S_OK);
    ASSERT_EQ(callback->OnReadSample(E_FAIL, 0, MF_SOURCE_READERF_ERROR, 42, original.Get()), S_OK);
    original.Reset();
    WebcamReadCallback::Sample sample;
    ASSERT_TRUE(callback->Wait({}, sample));
    EXPECT_EQ(sample.status, E_FAIL);
    EXPECT_EQ(sample.flags, MF_SOURCE_READERF_ERROR);
    EXPECT_EQ(sample.timestamp, 42);
    LONGLONG sample_time = 0;
    ASSERT_TRUE(sample.sample);
    ASSERT_EQ(sample.sample->GetSampleTime(&sample_time), S_OK);
    EXPECT_EQ(sample_time, 123);
    callback->Close();
    ASSERT_EQ(callback->OnReadSample(S_OK, 0, 0, 99, nullptr), S_OK);
    EXPECT_FALSE(callback->Wait({}, sample));
}

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

// ---------------------------------------------------------------------------
// ShouldOpenWebcamPreview (setup preview coupled to the enable state)
// ---------------------------------------------------------------------------

TEST(ShouldOpenWebcamPreviewTest, EnabledWithDeviceOpens) {
    EXPECT_TRUE(ShouldOpenWebcamPreview(/*enabled=*/true, /*has_device=*/true));
}

TEST(ShouldOpenWebcamPreviewTest, DisabledNeverOpens) {
    // The camera must not spring on just from opening the Webcam page.
    EXPECT_FALSE(ShouldOpenWebcamPreview(/*enabled=*/false, /*has_device=*/true));
}

TEST(ShouldOpenWebcamPreviewTest, NoDeviceNeverOpens) {
    EXPECT_FALSE(ShouldOpenWebcamPreview(/*enabled=*/true, /*has_device=*/false));
}

TEST(ShouldOpenWebcamPreviewTest, DisabledAndNoDeviceDoesNotOpen) {
    EXPECT_FALSE(ShouldOpenWebcamPreview(/*enabled=*/false, /*has_device=*/false));
}

// ---------------------------------------------------------------------------
// SelectBestWebcamNativeFormat (webcam fps was requested but never applied --
// OpenReader used to take the FIRST width/height match regardless of frame
// rate; now it picks the native entry whose fps is closest to what was asked).
// ---------------------------------------------------------------------------

TEST(SelectBestWebcamNativeFormatTest, NoWidthHeightMatchReturnsNegativeOne) {
    const std::vector<WebcamNativeFormat> formats = {
        {1280, 720, 30, 1},
        {1920, 1080, 30, 1},
    };
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 640, 480, 30), -1);
}

TEST(SelectBestWebcamNativeFormatTest, ExactFpsMatchWins) {
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 30, 1},
        {1920, 1080, 60, 1},
        {1920, 1080, 15, 1},
    };
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 1920, 1080, 60), 1);
}

TEST(SelectBestWebcamNativeFormatTest, NoExactFpsPicksClosestAvailable) {
    // A 1080p60 request against a device that only offers 1080p30/1080p24 at
    // that resolution must land on 30 (closest), not silently on whichever the
    // enumeration happens to list first.
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 24, 1},
        {1920, 1080, 30, 1},
    };
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 1920, 1080, 60), 1);
}

TEST(SelectBestWebcamNativeFormatTest, IgnoresEntriesAtOtherResolutions) {
    const std::vector<WebcamNativeFormat> formats = {
        {1280, 720, 60, 1}, // wrong resolution, exact fps -- must be ignored
        {1920, 1080, 30, 1},
    };
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 1920, 1080, 60), 1);
}

TEST(SelectBestWebcamNativeFormatTest, NonPositiveWantFpsTakesFirstWidthHeightMatch) {
    // want_fps <= 0 means "no frame-rate preference" -- preserves the pre-fix
    // behavior for any caller that does not care about frame rate.
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 60, 1},
        {1920, 1080, 30, 1},
    };
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 1920, 1080, 0), 0);
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 1920, 1080, -1), 0);
}

TEST(SelectBestWebcamNativeFormatTest, TieBreaksToFirstEnumeratedEntry) {
    // Equidistant from 45 fps (30 and 60): the first-enumerated entry wins.
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 30, 1},
        {1920, 1080, 60, 1},
    };
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 1920, 1080, 45), 0);
}

TEST(SelectBestWebcamNativeFormatTest, HandlesFractionalNtscFrameRates) {
    // 30000/1001 (~29.97 fps) must be distinguishable from a plain 30/1 entry.
    // A single-candidate list can't actually discriminate anything (it would
    // "win" no matter how the distance is computed); this list forces a real
    // choice between three candidates so a naive fps_num/fps_den truncation
    // (~29 fps) or an unreduced-fraction bug would visibly pick the wrong one.
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 24, 1},       // 24 fps -- clearly farther from 30 than NTSC 29.97
        {1920, 1080, 30000, 1001}, // ~29.97 fps -- the closest real candidate
        {1920, 1080, 60, 1},       // 60 fps -- farther than either of the above
    };
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 1920, 1080, 30), 1);
}

TEST(SelectBestWebcamNativeFormatTest, EmptyCandidateListReturnsNegativeOne) {
    EXPECT_EQ(SelectBestWebcamNativeFormat({}, 1920, 1080, 30), -1);
}

TEST(SelectBestWebcamNativeFormatTest, ZeroFpsDenIsTreatedAsOne) {
    // A malformed/degenerate MF_MT_FRAME_RATE ratio (den == 0) must not divide
    // by zero; it is treated as fps_num/1, same as a plain integer fps.
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 30, 0},
    };
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 1920, 1080, 30), 0);
}

// ---------------------------------------------------------------------------
// RankWebcamNativeFormats (the retry-order policy behind OpenReader's
// multi-candidate SetCurrentMediaType loop: the single closest-fps candidate
// can fail to negotiate for a reason unrelated to frame rate, so the reader
// retries progressively-less-ideal same-resolution candidates instead of
// giving up after one attempt).
// ---------------------------------------------------------------------------

TEST(RankWebcamNativeFormatsTest, OrdersByClosenessToWantFps) {
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 15, 1}, // index 0: farthest from 60
        {1920, 1080, 60, 1}, // index 1: exact match
        {1920, 1080, 30, 1}, // index 2: second-closest
    };
    const std::vector<int> ranked = RankWebcamNativeFormats(formats, 1920, 1080, 60);
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0], 1);
    EXPECT_EQ(ranked[1], 2);
    EXPECT_EQ(ranked[2], 0);
}

TEST(RankWebcamNativeFormatsTest, FirstElementMatchesSelectBestWebcamNativeFormat) {
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 24, 1},
        {1920, 1080, 30000, 1001},
        {1920, 1080, 60, 1},
    };
    const std::vector<int> ranked = RankWebcamNativeFormats(formats, 1920, 1080, 30);
    ASSERT_FALSE(ranked.empty());
    EXPECT_EQ(ranked.front(), SelectBestWebcamNativeFormat(formats, 1920, 1080, 30));
}

TEST(RankWebcamNativeFormatsTest, NoWidthHeightMatchReturnsEmpty) {
    const std::vector<WebcamNativeFormat> formats = {
        {1280, 720, 30, 1},
    };
    EXPECT_TRUE(RankWebcamNativeFormats(formats, 1920, 1080, 30).empty());
}

TEST(RankWebcamNativeFormatsTest, EmptyCandidateListReturnsEmpty) {
    EXPECT_TRUE(RankWebcamNativeFormats({}, 1920, 1080, 30).empty());
}

TEST(RankWebcamNativeFormatsTest, NonPositiveWantFpsKeepsEnumerationOrder) {
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 60, 1}, {1920, 1080, 30, 1}, {1280, 720, 30, 1}, // different resolution -- excluded
    };
    const std::vector<int> ranked = RankWebcamNativeFormats(formats, 1920, 1080, 0);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], 0);
    EXPECT_EQ(ranked[1], 1);
}

// ---------------------------------------------------------------------------
// RoundWebcamFps (the resolution combo label and the negotiated-fps report
// used to truncate a rational frame rate instead of rounding it -- e.g.
// 30000/1001, ~29.97 fps, displayed and stored as 29 instead of 30).
// ---------------------------------------------------------------------------

TEST(RoundWebcamFpsTest, RoundsNtscRateToNearestWholeFps) {
    EXPECT_EQ(RoundWebcamFps(30000, 1001), 30);
    EXPECT_EQ(RoundWebcamFps(60000, 1001), 60);
    EXPECT_EQ(RoundWebcamFps(24000, 1001), 24);
}

TEST(RoundWebcamFpsTest, PlainIntegerRatesAreUnaffected) {
    EXPECT_EQ(RoundWebcamFps(30, 1), 30);
    EXPECT_EQ(RoundWebcamFps(60, 1), 60);
}

TEST(RoundWebcamFpsTest, ZeroDenIsTreatedAsOne) {
    EXPECT_EQ(RoundWebcamFps(30, 0), 30);
}

TEST(RoundWebcamFpsTest, RoundsDownBelowHalf) {
    // 29 + 0.4 -> still rounds to 29, not 30 (exercises the "round", not
    // "always round up", direction).
    EXPECT_EQ(RoundWebcamFps(147, 5), 29); // 29.4 -> 29
    EXPECT_EQ(RoundWebcamFps(148, 5), 30); // 29.6 -> 30
}

} // namespace

// ---------------------------------------------------------------------------
// ShouldEnableWebcamToggle — the Record dock toggle follows the hardware.
// ---------------------------------------------------------------------------
TEST(ShouldEnableWebcamToggleTest, NoCameraLeavesNothingToTurnOn) {
    EXPECT_FALSE(ShouldEnableWebcamToggle(/*has_device=*/false, /*transport_locked=*/false));
}

TEST(ShouldEnableWebcamToggleTest, CameraAttachedEnablesTheToggle) {
    EXPECT_TRUE(ShouldEnableWebcamToggle(/*has_device=*/true, /*transport_locked=*/false));
}

TEST(ShouldEnableWebcamToggleTest, LockedTransportWinsOverAnAttachedCamera) {
    EXPECT_FALSE(ShouldEnableWebcamToggle(/*has_device=*/true, /*transport_locked=*/true));
}

TEST(ShouldEnableWebcamToggleTest, NoCameraAndLockedIsStillDisabled) {
    EXPECT_FALSE(ShouldEnableWebcamToggle(false, true));
}

// ---------------------------------------------------------------------------
// Native encoding preference and the packed frame layout.
// ---------------------------------------------------------------------------
TEST(RankWebcamNativeFormatsTest, EquallyCloseRatesPreferTheEncodingWithoutACpuDecode) {
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 30, 1, WebcamNativeEncoding::Other},
        {1920, 1080, 30, 1, WebcamNativeEncoding::Yuy2},
        {1920, 1080, 30, 1, WebcamNativeEncoding::Nv12},
    };
    EXPECT_EQ(RankWebcamNativeFormats(formats, 1920, 1080, 30), (std::vector<int>{2, 1, 0}));
}

TEST(RankWebcamNativeFormatsTest, TheRequestedRateOutranksTheEncoding) {
    // A USB 2 camera reaches 1080p30 only as MJPEG; its raw formats stop at 5 fps.
    const std::vector<WebcamNativeFormat> formats = {
        {1920, 1080, 5, 1, WebcamNativeEncoding::Yuy2},
        {1920, 1080, 30, 1, WebcamNativeEncoding::Other},
    };
    EXPECT_EQ(SelectBestWebcamNativeFormat(formats, 1920, 1080, 30), 1);
}

TEST(PackWebcamFrameTest, Nv12DropsRowPaddingFromBothPlanes) {
    // 4x2 frame with a 6-byte pitch: two luma rows, then one chroma row.
    const std::vector<uint8_t> padded = {1, 2, 3, 4, 0xEE, 0xEE, 5, 6, 7, 8, 0xEE, 0xEE, 9, 10, 11, 12, 0xEE, 0xEE};
    std::vector<uint8_t> packed;
    ASSERT_TRUE(PackWebcamFrame(exosnap::engine::WebcamPixelFormat::Nv12, 4, 2, padded.data(), 6, packed));
    EXPECT_EQ(packed, (std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
}

TEST(PackWebcamFrameTest, ABottomUpBgraBufferIsFlippedToTopDown) {
    const std::vector<uint8_t> bottom_up = {2, 2, 2, 2, 1, 1, 1, 1}; // 1x2, last row first in memory
    std::vector<uint8_t> packed;
    ASSERT_TRUE(PackWebcamFrame(exosnap::engine::WebcamPixelFormat::Bgra, 1, 2, bottom_up.data() + 4, -4, packed));
    EXPECT_EQ(packed, (std::vector<uint8_t>{1, 1, 1, 1, 2, 2, 2, 2}));
}

TEST(PackWebcamFrameTest, RejectsLayoutsThatCannotHoldTheFrame) {
    const std::vector<uint8_t> bytes(64, 0);
    std::vector<uint8_t> packed;
    EXPECT_FALSE(PackWebcamFrame(exosnap::engine::WebcamPixelFormat::Yuy2, 4, 2, bytes.data(), 6, packed))
        << "pitch shorter than a YUY2 row";
    EXPECT_FALSE(PackWebcamFrame(exosnap::engine::WebcamPixelFormat::Nv12, 3, 2, bytes.data(), 4, packed))
        << "NV12 needs even dimensions";
    EXPECT_FALSE(PackWebcamFrame(exosnap::engine::WebcamPixelFormat::Nv12, 4, 2, bytes.data() + 16, -4, packed))
        << "bottom-up YUV is not a layout cameras deliver";
}

// ---------------------------------------------------------------------------
// ResolveWebcamCaptureMode: what Settings shows and capture requests.
// ---------------------------------------------------------------------------
TEST(ResolveWebcamCaptureModeTest, AnOfferedRequestIsKept) {
    const std::vector<WebcamFormat> offered = {{1280, 720, 30, 1}, {1920, 1080, 30, 1}, {1920, 1080, 60, 1}};
    EXPECT_EQ(ResolveWebcamCaptureMode(offered, 1920, 1080, 60), (WebcamCaptureMode{1920, 1080, 60}));
}

TEST(ResolveWebcamCaptureModeTest, AMissingSizeTakesTheSmallestLargerSizeAndItsClosestRate) {
    const std::vector<WebcamFormat> offered = {
        {2560, 1440, 30, 1}, {640, 480, 30, 1}, {1920, 1080, 15, 1}, {1920, 1080, 25, 1}};
    EXPECT_EQ(ResolveWebcamCaptureMode(offered, 1280, 720, 30), (WebcamCaptureMode{1920, 1080, 25}));
    EXPECT_EQ(ResolveWebcamCaptureMode(offered, 3840, 2160, 30), (WebcamCaptureMode{2560, 1440, 30}))
        << "nothing large enough: the largest offered size";
    EXPECT_EQ(ResolveWebcamCaptureMode({{2560, 1440, 30, 1}}, 1280, 720, 60), (WebcamCaptureMode{2560, 1440, 30}));
}

TEST(ResolveWebcamCaptureModeTest, UnknownFormatsLeaveTheRequestAlone) {
    EXPECT_EQ(ResolveWebcamCaptureMode({}, 1280, 720, 30), (WebcamCaptureMode{1280, 720, 30}));
}

TEST(ResolveWebcamCaptureModeTest, NtscRatesAreOfferedAsWholeFramesPerSecond) {
    const std::vector<WebcamFormat> offered = {{1920, 1080, 30000, 1001}, {1920, 1080, 30, 1}};
    EXPECT_EQ(OfferedWebcamFrameRates(offered, 1920, 1080), (std::vector<int>{30}));
}

// ---------------------------------------------------------------------------
// UVC control values and settings.
// ---------------------------------------------------------------------------
TEST(WebcamControlsTest, AValueIsClampedIntoTheRangeAndOntoItsStepGrid) {
    WebcamControlRange range;
    range.minimum = 2800;
    range.maximum = 6500;
    range.step = 10;
    EXPECT_EQ(ClampWebcamControlValue(range, 1000), 2800);
    EXPECT_EQ(ClampWebcamControlValue(range, 9000), 6500);
    EXPECT_EQ(ClampWebcamControlValue(range, 4604), 4600);
    EXPECT_EQ(ClampWebcamControlValue(range, 4605), 4610);

    WebcamControlRange signed_range;
    signed_range.minimum = -64;
    signed_range.maximum = 64;
    EXPECT_EQ(ClampWebcamControlValue(signed_range, -12), -12);
}

TEST(WebcamControlsTest, SettingsAreOnePerControlInAFixedOrder) {
    const auto normalized = NormalizeWebcamControlSettings(
        {{WebcamControl::Zoom, false, 2}, {WebcamControl::Brightness, false, 1}, {WebcamControl::Zoom, false, 5}});
    ASSERT_EQ(normalized.size(), 2u);
    EXPECT_EQ(normalized[0].control, WebcamControl::Brightness);
    EXPECT_EQ(normalized[1].control, WebcamControl::Zoom);
    EXPECT_EQ(normalized[1].value, 5) << "the later edit wins";
}

TEST(WebcamControlsTest, EveryControlHasAStableKeyThatReadsBack) {
    for (const WebcamControl control : kAllWebcamControls) {
        ASSERT_FALSE(WebcamControlKey(control).empty());
        EXPECT_EQ(WebcamControlFromKey(WebcamControlKey(control)), control);
    }
    EXPECT_FALSE(WebcamControlFromKey("not_a_control").has_value());
}
