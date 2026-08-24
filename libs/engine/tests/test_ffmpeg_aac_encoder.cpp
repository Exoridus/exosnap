#include <gtest/gtest.h>

#include "ffmpeg_aac_encoder.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------------
// FfmpegAacEncoder tests.
//
// HONEST CAVEAT (read before trusting a green run): the AAC *encode* path in
// these tests can only execute when the linked avcodec actually has an AAC
// encoder compiled in. The prebuilt FFmpeg DLL currently pinned by
// cmake/VendorFFmpeg.cmake (exosnap-ffmpeg-build release r4) is a
// decoder+mux-only build with ZERO encoders, so avcodec_find_encoder(
// AV_CODEC_ID_AAC) returns null and FfmpegAacEncoder::Init() fails by design.
// Against that binary every encode-path test below GTEST_SKIPs itself, and the
// ONLY assertion that runs for real is the graceful-failure test
// (Init_MissingEncoder_FailsGracefullyNotCrash). Once the encoder-enabled r5
// release ships and VendorFFmpeg.cmake is repinned, the skipped tests exercise
// the real encoder. See ADR 0052.
// -----------------------------------------------------------------------------

namespace {

using exosnap::engine::EncodedAudioPacket;
using exosnap::engine::FfmpegAacEncoder;

constexpr uint32_t kSampleRate48 = 48000;
constexpr uint32_t kSampleRate441 = 44100;
constexpr uint32_t kChannelsStereo = 2;
constexpr uint32_t kChannelsMono = 1;

// True when Init() failed specifically because the avcodec build has no AAC
// encoder (the r4 case) rather than for some other, unexpected reason. The
// distinguishable marker is the avcodec_find_encoder mention in the message.
bool IsEncoderUnavailable(const std::string& err) {
    return err.find("avcodec_find_encoder") != std::string::npos;
}

// -------------------------------------------------------------------------
// Bitrate resolution — pure, always runnable regardless of encoder presence.
// -------------------------------------------------------------------------

TEST(FfmpegAacEncoderTest, ResolveBitrate_ZeroMapsToDefault192) {
    EXPECT_EQ(FfmpegAacEncoder::ResolveBitrateKbps(0), 192u);
}

TEST(FfmpegAacEncoderTest, ResolveBitrate_ClampsToValidRange) {
    EXPECT_EQ(FfmpegAacEncoder::ResolveBitrateKbps(32), 64u);   // below floor
    EXPECT_EQ(FfmpegAacEncoder::ResolveBitrateKbps(500), 320u); // above ceiling
    EXPECT_EQ(FfmpegAacEncoder::ResolveBitrateKbps(128), 128u); // in range
}

// -------------------------------------------------------------------------
// Graceful-failure contract — THE test that actually runs against r4.
//
// avcodec_find_encoder(AV_CODEC_ID_AAC) returning null must produce a clean,
// distinguishable Init() failure (false + a specific message), never a crash
// or a null-deref. This is the only AAC-encoder behavior the current binary
// can exercise.
// -------------------------------------------------------------------------

TEST(FfmpegAacEncoderTest, Init_MissingEncoder_FailsGracefullyNotCrash) {
    FfmpegAacEncoder encoder;
    std::string err;
    const bool ok = encoder.Init(kSampleRate48, kChannelsStereo, err);

    if (ok) {
        // The encoder-enabled build path: Init succeeded, so there is no
        // missing-encoder failure to assert. Extradata must exist instead.
        EXPECT_FALSE(encoder.CodecPrivateBytes().empty())
            << "A successful Init must publish AudioSpecificConfig extradata";
        encoder.Shutdown();
        GTEST_SKIP() << "AAC encoder present in this avcodec build; "
                        "missing-encoder path not exercised.";
    }

    EXPECT_FALSE(err.empty()) << "A failed Init must report a non-empty error";
    EXPECT_TRUE(IsEncoderUnavailable(err))
        << "Init failed for an unexpected reason (not the missing-encoder case): " << err;

    // Shutdown after a failed Init must be safe/idempotent.
    EXPECT_NO_FATAL_FAILURE(encoder.Shutdown());
}

TEST(FfmpegAacEncoderTest, FeedAndFlushAfterFailedInit_AreNoOps) {
    FfmpegAacEncoder encoder;
    std::string err;
    if (encoder.Init(kSampleRate48, kChannelsStereo, err)) {
        encoder.Shutdown();
        GTEST_SKIP() << "AAC encoder present; failed-init no-op path not exercised.";
    }

    // With no live encoder, Feed/Flush must not crash and must produce nothing.
    std::vector<float> input(2048, 0.0f);
    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated = 0;
    EXPECT_NO_FATAL_FAILURE(
        encoder.FeedFloat32(input.data(), input.size(), 0, accumulated, kSampleRate48, kChannelsStereo, packets));
    EXPECT_NO_FATAL_FAILURE(encoder.Flush(packets));
    EXPECT_TRUE(packets.empty()) << "No packets can be produced without an encoder";
}

// -------------------------------------------------------------------------
// Real-encode path — SKIPPED against r4, exercised once the encoder ships.
// Written now so the behavior is pinned the moment the DLL gains the encoder.
// -------------------------------------------------------------------------

class FfmpegAacEncodeParam : public ::testing::TestWithParam<std::pair<uint32_t, uint32_t>> {};

TEST_P(FfmpegAacEncodeParam, Init_ProducesNonEmptyExtradata) {
    const auto [rate, channels] = GetParam();
    FfmpegAacEncoder encoder;
    std::string err;
    if (!encoder.Init(rate, channels, err)) {
        if (IsEncoderUnavailable(err))
            GTEST_SKIP() << "AAC encoder not compiled into this avcodec build.";
        FAIL() << "Init failed unexpectedly: " << err;
    }
    EXPECT_FALSE(encoder.CodecPrivateBytes().empty()) << "AudioSpecificConfig extradata must be non-empty after Init";
    encoder.Shutdown();
}

TEST_P(FfmpegAacEncodeParam, FeedFullFrames_ProducesMonotonicPts) {
    const auto [rate, channels] = GetParam();
    FfmpegAacEncoder encoder;
    std::string err;
    if (!encoder.Init(rate, channels, err)) {
        if (IsEncoderUnavailable(err))
            GTEST_SKIP() << "AAC encoder not compiled into this avcodec build.";
        FAIL() << "Init failed unexpectedly: " << err;
    }

    // Feed 8 AAC-LC frames (8 x 1024 samples per channel).
    const size_t total = static_cast<size_t>(8) * 1024 * channels;
    std::vector<float> input(total, 0.0f);
    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated, rate, channels, packets);
    encoder.Flush(packets);

    ASSERT_GE(packets.size(), 2u) << "Expected multiple packets for 8 fed frames";
    for (size_t i = 1; i < packets.size(); ++i) {
        EXPECT_GT(packets[i].pts_ns, packets[i - 1].pts_ns) << "Packet PTS must strictly increase at packet " << i;
    }
    for (const auto& pkt : packets) {
        EXPECT_FALSE(pkt.bytes.empty()) << "Emitted packet must carry a payload";
    }
    encoder.Shutdown();
}

TEST_P(FfmpegAacEncodeParam, ConfiguredBitrate_InitSucceeds) {
    const auto [rate, channels] = GetParam();
    FfmpegAacEncoder encoder;
    encoder.SetBitrateKbps(128);
    std::string err;
    if (!encoder.Init(rate, channels, err)) {
        if (IsEncoderUnavailable(err))
            GTEST_SKIP() << "AAC encoder not compiled into this avcodec build.";
        FAIL() << "Init with a configured bitrate failed unexpectedly: " << err;
    }
    EXPECT_FALSE(encoder.CodecPrivateBytes().empty());
    encoder.Shutdown();
}

TEST_P(FfmpegAacEncodeParam, PartialFrameFlush_DrainsCleanly) {
    const auto [rate, channels] = GetParam();
    FfmpegAacEncoder encoder;
    std::string err;
    if (!encoder.Init(rate, channels, err)) {
        if (IsEncoderUnavailable(err))
            GTEST_SKIP() << "AAC encoder not compiled into this avcodec build.";
        FAIL() << "Init failed unexpectedly: " << err;
    }

    // Feed a partial frame (< 1024 samples per channel) then flush.
    const size_t partial = static_cast<size_t>(512) * channels;
    std::vector<float> input(partial, 0.0f);
    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated, rate, channels, packets);
    EXPECT_NO_FATAL_FAILURE(encoder.Flush(packets));
    encoder.Shutdown();
}

INSTANTIATE_TEST_SUITE_P(RatesAndChannels, FfmpegAacEncodeParam,
                         ::testing::Values(std::make_pair(kSampleRate48, kChannelsStereo),
                                           std::make_pair(kSampleRate441, kChannelsStereo),
                                           std::make_pair(kSampleRate48, kChannelsMono),
                                           std::make_pair(kSampleRate441, kChannelsMono)));

TEST(FfmpegAacEncoderTest, Shutdown_IsIdempotent) {
    FfmpegAacEncoder encoder;
    EXPECT_NO_FATAL_FAILURE(encoder.Shutdown());
    EXPECT_NO_FATAL_FAILURE(encoder.Shutdown());
}

} // namespace
