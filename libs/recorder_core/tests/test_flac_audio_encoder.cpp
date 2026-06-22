// Unit tests for FlacAudioEncoder (0.6.0 Audio v2): lossless FLAC via libFLAC.
//
// These tests link libFLAC (the encoder is NOT a pure-logic component). They
// assert plumbing/markers/sizes rather than decoding playback: Init/Feed/Flush
// produce non-empty output, the CodecPrivate starts with the native "fLaC"
// marker and is non-empty, the float→int16 conversion matches the PCM mapping,
// PTS/frame-counter advance, and degenerate inputs are handled. No GPU / MF.

#include <gtest/gtest.h>

#include "flac_audio_encoder.h"

#include <cmath>
#include <cstdint>
#include <vector>

using recorder_core::EncodedAudioPacket;
using recorder_core::FlacAudioEncoder;

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;

// "fLaC" stream marker.
constexpr uint8_t kFlacMarker[4] = {0x66, 0x4C, 0x61, 0x43};

// Feed `frames` of interleaved stereo silence (or a sine, if amplitude > 0) in
// `chunk_frames`-sized buffers and collect all emitted packets.
std::vector<EncodedAudioPacket> EncodeSignal(FlacAudioEncoder& enc, uint64_t& accumulated_frames, size_t frames,
                                             size_t chunk_frames, float amplitude) {
    std::vector<EncodedAudioPacket> all;
    std::vector<float> buf;
    size_t emitted = 0;
    while (emitted < frames) {
        const size_t n = std::min(chunk_frames, frames - emitted);
        buf.resize(n * kChannels);
        for (size_t i = 0; i < n; ++i) {
            float s = 0.0f;
            if (amplitude > 0.0f) {
                const double t = static_cast<double>(emitted + i) / kSampleRate;
                s = amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * t));
            }
            buf[i * kChannels] = s;
            buf[i * kChannels + 1] = s;
        }
        std::vector<EncodedAudioPacket> out;
        enc.FeedFloat32(buf.data(), buf.size(), 0, accumulated_frames, kSampleRate, kChannels, out);
        for (auto& p : out)
            all.push_back(std::move(p));
        emitted += n;
    }
    return all;
}

// ---------------------------------------------------------------------------
// Float32ToS16 — static conversion (identical mapping to the PCM path)
// ---------------------------------------------------------------------------

TEST(FlacAudioEncoder, Float32ToS16_Silence_IsZero) {
    EXPECT_EQ(FlacAudioEncoder::Float32ToS16(0.0f), 0);
}

TEST(FlacAudioEncoder, Float32ToS16_FullScale) {
    EXPECT_EQ(FlacAudioEncoder::Float32ToS16(1.0f), 32767);
    EXPECT_EQ(FlacAudioEncoder::Float32ToS16(-1.0f), -32767);
}

TEST(FlacAudioEncoder, Float32ToS16_Clamps) {
    EXPECT_EQ(FlacAudioEncoder::Float32ToS16(2.5f), 32767);
    EXPECT_EQ(FlacAudioEncoder::Float32ToS16(-3.0f), -32767);
}

TEST(FlacAudioEncoder, Float32ToS16_RoundsToNearest) {
    EXPECT_EQ(FlacAudioEncoder::Float32ToS16(0.5f), 16384);
    EXPECT_EQ(FlacAudioEncoder::Float32ToS16(-0.5f), -16384);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

TEST(FlacAudioEncoder, Init_ValidParams_Succeeds) {
    FlacAudioEncoder enc;
    std::string err;
    EXPECT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;
    EXPECT_TRUE(err.empty());
}

TEST(FlacAudioEncoder, Init_ZeroSampleRate_Fails) {
    FlacAudioEncoder enc;
    std::string err;
    EXPECT_FALSE(enc.Init(0, kChannels, err));
    EXPECT_FALSE(err.empty());
}

TEST(FlacAudioEncoder, Init_ZeroChannels_Fails) {
    FlacAudioEncoder enc;
    std::string err;
    EXPECT_FALSE(enc.Init(kSampleRate, 0, err));
    EXPECT_FALSE(err.empty());
}

// ---------------------------------------------------------------------------
// CodecPrivate — native fLaC header
// ---------------------------------------------------------------------------

TEST(FlacAudioEncoder, CodecPrivate_StartsWithFlacMarker) {
    FlacAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;

    const auto cp = enc.CodecPrivateBytes();
    ASSERT_GE(cp.size(), 4u);
    EXPECT_EQ(cp[0], kFlacMarker[0]);
    EXPECT_EQ(cp[1], kFlacMarker[1]);
    EXPECT_EQ(cp[2], kFlacMarker[2]);
    EXPECT_EQ(cp[3], kFlacMarker[3]);
    // The header must also carry at least the STREAMINFO block (4-byte marker +
    // 4-byte block header + 34-byte STREAMINFO payload = 42 bytes minimum).
    EXPECT_GE(cp.size(), 42u);
}

// ---------------------------------------------------------------------------
// Feed / Flush — produces non-empty output
// ---------------------------------------------------------------------------

TEST(FlacAudioEncoder, Encode_Sine_ProducesNonEmptyFrames) {
    FlacAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;

    uint64_t frames = 0;
    // 1 second of a 440 Hz tone, fed in 1024-frame chunks.
    auto packets = EncodeSignal(enc, frames, kSampleRate, 1024, 0.5f);

    std::vector<EncodedAudioPacket> drain;
    enc.Flush(drain);
    for (auto& p : drain)
        packets.push_back(std::move(p));

    ASSERT_FALSE(packets.empty()) << "FLAC encoder produced no frames";
    size_t total_bytes = 0;
    uint64_t prev_pts = 0;
    bool first = true;
    for (const auto& p : packets) {
        EXPECT_FALSE(p.bytes.empty());
        total_bytes += p.bytes.size();
        if (!first) {
            EXPECT_GE(p.pts_ns, prev_pts) << "PTS must be monotonically non-decreasing";
        }
        prev_pts = p.pts_ns;
        first = false;
    }
    EXPECT_GT(total_bytes, 0u);
    EXPECT_EQ(frames, static_cast<uint64_t>(kSampleRate));
}

TEST(FlacAudioEncoder, Encode_Silence_FinishesWithValidStream) {
    FlacAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;

    uint64_t frames = 0;
    // 0.5 s of silence.
    auto packets = EncodeSignal(enc, frames, kSampleRate / 2, 4096, 0.0f);

    std::vector<EncodedAudioPacket> drain;
    enc.Flush(drain);
    for (auto& p : drain)
        packets.push_back(std::move(p));

    // Even pure silence must yield at least one FLAC audio frame after finish.
    ASSERT_FALSE(packets.empty());
    EXPECT_EQ(frames, static_cast<uint64_t>(kSampleRate / 2));
}

TEST(FlacAudioEncoder, FirstPacketPts_IsZero) {
    FlacAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;

    uint64_t frames = 0;
    auto packets = EncodeSignal(enc, frames, kSampleRate, 4096, 0.3f);
    std::vector<EncodedAudioPacket> drain;
    enc.Flush(drain);
    for (auto& p : drain)
        packets.push_back(std::move(p));

    ASSERT_FALSE(packets.empty());
    EXPECT_EQ(packets.front().pts_ns, 0u);
}

// ---------------------------------------------------------------------------
// Degenerate inputs — no packet, no crash
// ---------------------------------------------------------------------------

TEST(FlacAudioEncoder, Feed_NullData_NoPacketNoAdvance) {
    FlacAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;

    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(nullptr, 4, 0, frames, kSampleRate, kChannels, out);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(frames, 0u);
}

TEST(FlacAudioEncoder, Feed_MismatchedFormat_NoPacket) {
    FlacAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;

    const std::vector<float> in(2048, 0.25f);
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, 44100, kChannels, out); // wrong rate
    EXPECT_TRUE(out.empty());
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, 1, out); // wrong channels
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(frames, 0u);
}

// ---------------------------------------------------------------------------
// ADR 0030: SetBitDepth — 24-bit
// ---------------------------------------------------------------------------

TEST(FlacAudioEncoder, SetBitDepth24_CodecPrivateStartsWithFlacMarker) {
    FlacAudioEncoder enc;
    enc.SetBitDepth(24);
    EXPECT_EQ(enc.BitsPerSample(), 24u);
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;

    const auto cp = enc.CodecPrivateBytes();
    ASSERT_GE(cp.size(), 42u); // fLaC marker + STREAMINFO
    EXPECT_EQ(cp[0], kFlacMarker[0]);
    EXPECT_EQ(cp[1], kFlacMarker[1]);
    EXPECT_EQ(cp[2], kFlacMarker[2]);
    EXPECT_EQ(cp[3], kFlacMarker[3]);
}

TEST(FlacAudioEncoder, SetBitDepth24_ProducesFrames) {
    FlacAudioEncoder enc;
    enc.SetBitDepth(24);
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;

    uint64_t frames = 0;
    auto packets = EncodeSignal(enc, frames, kSampleRate, 1024, 0.5f);
    std::vector<EncodedAudioPacket> drain;
    enc.Flush(drain);
    for (auto& p : drain)
        packets.push_back(std::move(p));

    ASSERT_FALSE(packets.empty()) << "FLAC-24 encoder produced no frames";
}

// ---------------------------------------------------------------------------
// ADR 0030: SetCompressionLevel — non-default level (level 0 = fastest)
// ---------------------------------------------------------------------------

TEST(FlacAudioEncoder, SetCompressionLevel0_ProducesFrames) {
    FlacAudioEncoder enc;
    enc.SetCompressionLevel(0); // fastest, least compression
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err)) << err;

    const auto cp = enc.CodecPrivateBytes();
    ASSERT_GE(cp.size(), 4u);
    EXPECT_EQ(cp[0], kFlacMarker[0]);

    uint64_t frames = 0;
    auto packets = EncodeSignal(enc, frames, kSampleRate / 2, 4096, 0.3f);
    std::vector<EncodedAudioPacket> drain;
    enc.Flush(drain);
    for (auto& p : drain)
        packets.push_back(std::move(p));

    ASSERT_FALSE(packets.empty()) << "FLAC level-0 encoder produced no frames";
}

// ---------------------------------------------------------------------------
// ADR 0030: Float32ToInt static helper
// ---------------------------------------------------------------------------

TEST(FlacAudioEncoder, Float32ToInt_16bit_SameAsFloat32ToS16) {
    EXPECT_EQ(FlacAudioEncoder::Float32ToInt(1.0f, 16), FlacAudioEncoder::Float32ToS16(1.0f));
    EXPECT_EQ(FlacAudioEncoder::Float32ToInt(-1.0f, 16), FlacAudioEncoder::Float32ToS16(-1.0f));
    EXPECT_EQ(FlacAudioEncoder::Float32ToInt(0.0f, 16), 0);
}

TEST(FlacAudioEncoder, Float32ToInt_24bit_FullScale) {
    EXPECT_EQ(FlacAudioEncoder::Float32ToInt(1.0f, 24), 8388607);
    EXPECT_EQ(FlacAudioEncoder::Float32ToInt(-1.0f, 24), -8388607);
    EXPECT_EQ(FlacAudioEncoder::Float32ToInt(0.0f, 24), 0);
}

TEST(FlacAudioEncoder, Float32ToInt_24bit_ClampsOutOfRange) {
    EXPECT_EQ(FlacAudioEncoder::Float32ToInt(2.0f, 24), 8388607);
    EXPECT_EQ(FlacAudioEncoder::Float32ToInt(-3.0f, 24), -8388607);
}

} // namespace
