// Pure classifier for a WASAPI *loopback* acquire result — the SYS/system-
// audio mirror of ClassifyWasapiAcquireFailure (wasapi_capture_src.h).
//
// This pins the fix for a silent-death bug: the SYS loopback track (full
// system output via WasapiLoopback/WasapiLoopbackSrc, used for screen-capture
// system audio) used to swallow every IAudioCaptureClient::GetBuffer failure
// identically — including AUDCLNT_E_DEVICE_INVALIDATED on a render-endpoint
// unplug/replug or default-device switch — so the track just stopped
// producing data with no error anywhere, contradicting the diagnostics-first
// posture. GetBuffer/GetNextPacketSize cannot be mocked without live COM
// objects, so the HRESULT-classification + message-formatting logic is
// extracted into the pure ClassifyLoopbackAcquire() function and pinned here;
// the real unplug remains a live/manual check (see PR description).

#include "wasapi_loopback.h"
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>

#include <Audioclient.h>
#include <gtest/gtest.h>

#include <string>

using namespace exosnap::engine;

namespace {

TEST(WasapiLoopbackAcquire, SuccessIsNotFatal) {
    const auto result = ClassifyLoopbackAcquire("IAudioCaptureClient::GetBuffer", S_OK);
    EXPECT_FALSE(result.fatal);
    EXPECT_TRUE(result.error_message.empty());
}

TEST(WasapiLoopbackAcquire, BufferEmptyIsNotFatal) {
    // No packet ready this poll tick — benign, must not end the track.
    const auto result = ClassifyLoopbackAcquire("IAudioCaptureClient::GetNextPacketSize", AUDCLNT_S_BUFFER_EMPTY);
    EXPECT_FALSE(result.fatal);
    EXPECT_TRUE(result.error_message.empty());
}

TEST(WasapiLoopbackAcquire, DeviceInvalidatedIsFatalAndReported) {
    // The finding this fixes: a vanished/changed default render endpoint must
    // surface as a real error instead of silencing the track.
    const auto result = ClassifyLoopbackAcquire("IAudioCaptureClient::GetBuffer", AUDCLNT_E_DEVICE_INVALIDATED);
    EXPECT_TRUE(result.fatal);
    EXPECT_NE(result.error_message.find("AUDCLNT_E_DEVICE_INVALIDATED"), std::string::npos);
    EXPECT_NE(result.error_message.find("IAudioCaptureClient::GetBuffer"), std::string::npos);
}

TEST(WasapiLoopbackAcquire, ServiceNotRunningIsFatalAndReported) {
    const auto result =
        ClassifyLoopbackAcquire("IAudioCaptureClient::GetNextPacketSize", AUDCLNT_E_SERVICE_NOT_RUNNING);
    EXPECT_TRUE(result.fatal);
    EXPECT_NE(result.error_message.find("AUDCLNT_E_SERVICE_NOT_RUNNING"), std::string::npos);
}

TEST(WasapiLoopbackAcquire, UnexpectedHresultFailsClosed) {
    // Fail-closed: any HRESULT we did not explicitly reason about must still be
    // reported rather than silently treated as benign.
    const auto result = ClassifyLoopbackAcquire("IAudioCaptureClient::GetBuffer", AUDCLNT_E_BUFFER_ERROR);
    EXPECT_TRUE(result.fatal);
    EXPECT_FALSE(result.error_message.empty());
}

} // namespace

TEST(WasapiNativeFormat, PreservesFloatRateAndSurroundChannels) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nSamplesPerSec = 44100;
    format.nChannels = 6;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 24;
    format.nAvgBytesPerSec = 44100 * 24;
    const auto native = ClassifyNativeWasapiFormat(format);
    ASSERT_TRUE(native);
    EXPECT_EQ(native->sample_rate, 44100u);
    EXPECT_EQ(native->channels, 6u);
    EXPECT_EQ(native->sample_format, AudioSampleFormat::Float32);
    format.nBlockAlign = 4;
    EXPECT_FALSE(ClassifyNativeWasapiFormat(format));
}

TEST(WasapiNativeFormat, RejectsUnknownExtensibleGuidAndInvalidValidBits) {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.cbSize = 22;
    format.Format.nSamplesPerSec = 96000;
    format.Format.nChannels = 2;
    format.Format.wBitsPerSample = 16;
    format.Format.nBlockAlign = 4;
    format.Format.nAvgBytesPerSec = 384000;
    format.Samples.wValidBitsPerSample = 16;
    const GUID pcm = {1, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    format.SubFormat = pcm;
    ASSERT_TRUE(ClassifyNativeWasapiFormat(format.Format));
    format.SubFormat.Data4[7] ^= 1;
    EXPECT_FALSE(ClassifyNativeWasapiFormat(format.Format));
    format.SubFormat = pcm;
    format.Samples.wValidBitsPerSample = 12;
    EXPECT_FALSE(ClassifyNativeWasapiFormat(format.Format));
    format.Samples.wValidBitsPerSample = 16;
    format.Format.cbSize = 0;
    EXPECT_FALSE(ClassifyNativeWasapiFormat(format.Format));
}