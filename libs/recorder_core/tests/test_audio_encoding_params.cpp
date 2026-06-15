// Tests for audio encoding parameters (ADR 0019):
//   - OpusFrameDuration → sample count mapping
//   - PTS correctness at 10 ms frame size
//   - Opus bitrate clamping
//   - SetEncodingParams round-trip through Init
//   - FDK-AAC / MF-AAC bitrate clamping helpers
//   - FrameSizeSamples accessor

#include <gtest/gtest.h>

#include "opus_audio_encoder.h"

#if EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC
#include "fdk_aac_encoder.h"
#include "mf_aac_encoder.h"
#endif

#include <recorder_core/recorder_session.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using recorder_core::EncodedAudioPacket;
using recorder_core::OpusAudioEncoder;
using recorder_core::OpusFrameDuration;
using recorder_core::OpusFrameSizeSamples;

#if EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC
using recorder_core::FdkAacEncoder;
using recorder_core::MfAacEncoder;
#endif

// ---------------------------------------------------------------------------
// OpusFrameDuration → sample-count mapping
// ---------------------------------------------------------------------------

TEST(AudioEncodingParamsTest, OpusFrameSizeSamples_Ms20_Is960) {
    EXPECT_EQ(OpusFrameSizeSamples(OpusFrameDuration::Ms20), 960);
}

TEST(AudioEncodingParamsTest, OpusFrameSizeSamples_Ms10_Is480) {
    EXPECT_EQ(OpusFrameSizeSamples(OpusFrameDuration::Ms10), 480);
}

TEST(AudioEncodingParamsTest, OpusFrameSizeSamples_Ms5_Is240) {
    EXPECT_EQ(OpusFrameSizeSamples(OpusFrameDuration::Ms5), 240);
}

TEST(AudioEncodingParamsTest, OpusFrameSizeSamples_Ms2_5_Is120) {
    EXPECT_EQ(OpusFrameSizeSamples(OpusFrameDuration::Ms2_5), 120);
}

// ---------------------------------------------------------------------------
// FrameSizeSamples accessor — reflects the value set before Init
// ---------------------------------------------------------------------------

TEST(AudioEncodingParamsTest, Opus_DefaultFrameSize_Is960) {
    OpusAudioEncoder encoder;
    // Default frame duration is Ms20 → 960 samples
    EXPECT_EQ(encoder.FrameSizeSamples(), 960);
}

TEST(AudioEncodingParamsTest, Opus_SetFrameDuration_Ms10_ReflectedBeforeInit) {
    OpusAudioEncoder encoder;
    encoder.SetEncodingParams(160, OpusFrameDuration::Ms10, 10);
    EXPECT_EQ(encoder.FrameSizeSamples(), 480);
}

TEST(AudioEncodingParamsTest, Opus_SetFrameDuration_Ms10_ReflectedAfterInit) {
    OpusAudioEncoder encoder;
    encoder.SetEncodingParams(160, OpusFrameDuration::Ms10, 10);
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));
    EXPECT_EQ(encoder.FrameSizeSamples(), 480);
    encoder.Shutdown();
}

// ---------------------------------------------------------------------------
// PTS correctness at 10 ms frame size (480 samples @ 48 kHz)
// Each packet must be exactly 10 000 000 ns apart.
// ---------------------------------------------------------------------------

TEST(AudioEncodingParamsTest, Opus_PtsStep_10ms_PerFrame) {
    OpusAudioEncoder encoder;
    encoder.SetEncodingParams(160, OpusFrameDuration::Ms10, 10);
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));

    // Feed 6 complete 10 ms frames (6 × 480 × 2 = 5760 floats).
    uint64_t accumulated_frames = 0;
    std::vector<EncodedAudioPacket> packets;
    std::vector<float> input(6u * 480u * 2u, 0.0f);
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, 48000, 2, packets);

    ASSERT_EQ(packets.size(), 6u);
    constexpr uint64_t kStep = 10'000'000ULL; // 10 ms in ns
    for (std::size_t i = 0; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].pts_ns, i * kStep) << "Packet " << i << " has wrong PTS";
    }
    encoder.Shutdown();
}

// ---------------------------------------------------------------------------
// Bitrate clamping — Opus
// ClampOpusBitrateKbps: 0 → 0 (auto), below min → 32, above max → 510
// ---------------------------------------------------------------------------

TEST(AudioEncodingParamsTest, OpusBitrateClamp_Zero_IsAuto) {
    EXPECT_EQ(OpusAudioEncoder::ClampOpusBitrateKbps(0u), 0u);
}

TEST(AudioEncodingParamsTest, OpusBitrateClamp_BelowMin_ClampsTo32) {
    EXPECT_EQ(OpusAudioEncoder::ClampOpusBitrateKbps(5u), 32u);
}

TEST(AudioEncodingParamsTest, OpusBitrateClamp_Min_IsPassedThrough) {
    EXPECT_EQ(OpusAudioEncoder::ClampOpusBitrateKbps(32u), 32u);
}

TEST(AudioEncodingParamsTest, OpusBitrateClamp_Valid_IsPassedThrough) {
    EXPECT_EQ(OpusAudioEncoder::ClampOpusBitrateKbps(160u), 160u);
}

TEST(AudioEncodingParamsTest, OpusBitrateClamp_Max_IsPassedThrough) {
    EXPECT_EQ(OpusAudioEncoder::ClampOpusBitrateKbps(510u), 510u);
}

TEST(AudioEncodingParamsTest, OpusBitrateClamp_AboveMax_ClampsTo510) {
    EXPECT_EQ(OpusAudioEncoder::ClampOpusBitrateKbps(9999u), 510u);
}

// ---------------------------------------------------------------------------
// Bitrate clamping — FDK-AAC (full build only)
// ResolveBitrateKbps: 0 → 192 (default), below min → 64, above max → 320
// ---------------------------------------------------------------------------

#if EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC

TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_Zero_IsDefault192) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(0u), 192u);
}

TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_BelowMin_ClampsTo64) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(10u), 64u);
}

TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_Valid_IsPassedThrough) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(192u), 192u);
}

TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_Max_IsPassedThrough) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(320u), 320u);
}

TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_AboveMax_ClampsTo320) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(999u), 320u);
}

// ---------------------------------------------------------------------------
// Bitrate clamping — MF AAC (full build only)
// ---------------------------------------------------------------------------

TEST(AudioEncodingParamsTest, MfAacBitrateResolve_Zero_IsDefault192) {
    EXPECT_EQ(MfAacEncoder::ResolveBitrateKbps(0u), 192u);
}

TEST(AudioEncodingParamsTest, MfAacBitrateResolve_BelowMin_ClampsTo64) {
    EXPECT_EQ(MfAacEncoder::ResolveBitrateKbps(10u), 64u);
}

TEST(AudioEncodingParamsTest, MfAacBitrateResolve_AboveMax_ClampsTo320) {
    EXPECT_EQ(MfAacEncoder::ResolveBitrateKbps(999u), 320u);
}

#endif // EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC

// ---------------------------------------------------------------------------
// SetEncodingParams + Init at default values must succeed (regression guard)
// ---------------------------------------------------------------------------

TEST(AudioEncodingParamsTest, Opus_DefaultParams_InitSucceeds) {
    OpusAudioEncoder encoder;
    encoder.SetEncodingParams(160, OpusFrameDuration::Ms20, 10);
    std::string err;
    EXPECT_TRUE(encoder.Init(48000, 2, err));
    encoder.Shutdown();
}

TEST(AudioEncodingParamsTest, Opus_Complexity0_InitSucceeds) {
    OpusAudioEncoder encoder;
    encoder.SetEncodingParams(160, OpusFrameDuration::Ms20, 0);
    std::string err;
    EXPECT_TRUE(encoder.Init(48000, 2, err));
    encoder.Shutdown();
}

TEST(AudioEncodingParamsTest, Opus_Ms5_InitSucceeds) {
    OpusAudioEncoder encoder;
    encoder.SetEncodingParams(128, OpusFrameDuration::Ms5, 10);
    std::string err;
    EXPECT_TRUE(encoder.Init(48000, 2, err));
    encoder.Shutdown();
}

TEST(AudioEncodingParamsTest, Opus_Ms2_5_InitSucceeds) {
    OpusAudioEncoder encoder;
    encoder.SetEncodingParams(64, OpusFrameDuration::Ms2_5, 5);
    std::string err;
    EXPECT_TRUE(encoder.Init(48000, 2, err));
    encoder.Shutdown();
}

} // namespace
