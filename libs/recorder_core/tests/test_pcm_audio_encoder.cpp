// Unit tests for PcmAudioEncoder (0.6.0 Audio v2): float -> S16LE passthrough.
//
// Covers: Float32ToS16 conversion (full-scale, clamp beyond +/-1, round-to-
// nearest, silence), packet sizing / byte layout (interleaved S16LE), PTS and
// frame-counter advancement, empty CodecPrivate, and degenerate inputs. No
// FFmpeg / NVENC / GPU is needed — the encoder is a pure sample-format conversion.

#include <gtest/gtest.h>

#include "pcm_audio_encoder.h"

#include <cstdint>
#include <vector>

using recorder_core::EncodedAudioPacket;
using recorder_core::PcmAudioEncoder;

namespace {

// Read an interleaved S16LE sample at sample index `i` from packet bytes.
int16_t SampleAt(const std::vector<uint8_t>& bytes, size_t i) {
    const auto lo = static_cast<uint16_t>(bytes[i * 2]);
    const auto hi = static_cast<uint16_t>(bytes[i * 2 + 1]);
    return static_cast<int16_t>(static_cast<uint16_t>(lo | (hi << 8u)));
}

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;

// ---------------------------------------------------------------------------
// Float32ToS16 — static conversion
// ---------------------------------------------------------------------------

TEST(PcmAudioEncoder, Float32ToS16_Silence_IsZero) {
    EXPECT_EQ(PcmAudioEncoder::Float32ToS16(0.0f), 0);
}

TEST(PcmAudioEncoder, Float32ToS16_FullScalePositive_Is32767) {
    EXPECT_EQ(PcmAudioEncoder::Float32ToS16(1.0f), 32767);
}

TEST(PcmAudioEncoder, Float32ToS16_FullScaleNegative_IsMinus32767) {
    EXPECT_EQ(PcmAudioEncoder::Float32ToS16(-1.0f), -32767);
}

TEST(PcmAudioEncoder, Float32ToS16_ClampsAboveOne) {
    EXPECT_EQ(PcmAudioEncoder::Float32ToS16(2.5f), 32767);
}

TEST(PcmAudioEncoder, Float32ToS16_ClampsBelowMinusOne) {
    // Below -1.0 clamps to -1.0 -> -32767 (not the asymmetric -32768).
    EXPECT_EQ(PcmAudioEncoder::Float32ToS16(-3.0f), -32767);
}

TEST(PcmAudioEncoder, Float32ToS16_RoundsToNearest) {
    // 0.5 * 32767 = 16383.5 -> rounds to 16384.
    EXPECT_EQ(PcmAudioEncoder::Float32ToS16(0.5f), 16384);
    // -0.5 -> -16383.5 -> rounds away from zero to -16384 (lround).
    EXPECT_EQ(PcmAudioEncoder::Float32ToS16(-0.5f), -16384);
}

TEST(PcmAudioEncoder, Float32ToS16_SmallPositive_RoundsToOne) {
    // 0.00002 * 32767 ~= 0.655 -> rounds to 1.
    EXPECT_EQ(PcmAudioEncoder::Float32ToS16(0.00002f), 1);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

TEST(PcmAudioEncoder, Init_ValidParams_Succeeds) {
    PcmAudioEncoder enc;
    std::string err;
    EXPECT_TRUE(enc.Init(kSampleRate, kChannels, err));
    EXPECT_TRUE(err.empty());
}

TEST(PcmAudioEncoder, Init_ZeroSampleRate_Fails) {
    PcmAudioEncoder enc;
    std::string err;
    EXPECT_FALSE(enc.Init(0, kChannels, err));
    EXPECT_FALSE(err.empty());
}

TEST(PcmAudioEncoder, Init_ZeroChannels_Fails) {
    PcmAudioEncoder enc;
    std::string err;
    EXPECT_FALSE(enc.Init(kSampleRate, 0, err));
    EXPECT_FALSE(err.empty());
}

// ---------------------------------------------------------------------------
// CodecPrivate — A_PCM/INT_LIT carries none
// ---------------------------------------------------------------------------

TEST(PcmAudioEncoder, CodecPrivate_IsEmpty) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));
    EXPECT_TRUE(enc.CodecPrivateBytes().empty());
}

// ---------------------------------------------------------------------------
// FeedFloat32 — packet production, sizing, byte layout
// ---------------------------------------------------------------------------

TEST(PcmAudioEncoder, Feed_OneBuffer_ProducesOnePacketWithS16Bytes) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    // One stereo frame: L=+1.0, R=-1.0.
    const std::vector<float> in = {1.0f, -1.0f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, kChannels, out);

    ASSERT_EQ(out.size(), 1u);
    // 2 interleaved samples * 2 bytes each = 4 bytes.
    ASSERT_EQ(out[0].bytes.size(), 4u);
    EXPECT_EQ(SampleAt(out[0].bytes, 0), 32767);  // L
    EXPECT_EQ(SampleAt(out[0].bytes, 1), -32767); // R
    EXPECT_EQ(out[0].pts_ns, 0u);
}

TEST(PcmAudioEncoder, Feed_LittleEndianByteOrder) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    // 0.5 -> 16384 = 0x4000 -> LE bytes {0x00, 0x40}.
    const std::vector<float> in = {0.5f, 0.5f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, kChannels, out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].bytes.size(), 4u);
    EXPECT_EQ(out[0].bytes[0], 0x00u);
    EXPECT_EQ(out[0].bytes[1], 0x40u);
}

TEST(PcmAudioEncoder, Feed_AdvancesFrameCounterByFrameCount) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    // 480 stereo frames = 960 interleaved samples = 10 ms at 48 kHz.
    const std::vector<float> in(960, 0.0f);
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, kChannels, out);

    EXPECT_EQ(frames, 480u);
}

TEST(PcmAudioEncoder, Feed_SecondBuffer_PtsDerivedFromAccumulatedFrames) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    const std::vector<float> in(960, 0.0f); // 480 frames per call
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;

    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, kChannels, out);
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, kChannels, out);

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].pts_ns, 0u);
    // Second packet starts at 480 frames -> 480 / 48000 s = 10 ms = 10,000,000 ns.
    EXPECT_EQ(out[1].pts_ns, 10000000u);
    EXPECT_EQ(frames, 960u);
}

TEST(PcmAudioEncoder, Feed_Silence_ProducesZeroSamples) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    const std::vector<float> in(8, 0.0f);
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, kChannels, out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].bytes.size(), 16u);
    for (uint8_t b : out[0].bytes) {
        EXPECT_EQ(b, 0u);
    }
}

// ---------------------------------------------------------------------------
// Degenerate inputs — no packet, no crash
// ---------------------------------------------------------------------------

TEST(PcmAudioEncoder, Feed_NullData_NoPacket) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(nullptr, 4, 0, frames, kSampleRate, kChannels, out);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(frames, 0u);
}

TEST(PcmAudioEncoder, Feed_ZeroSamples_NoPacket) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    const std::vector<float> in = {0.0f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), 0, 0, frames, kSampleRate, kChannels, out);
    EXPECT_TRUE(out.empty());
}

TEST(PcmAudioEncoder, Feed_MismatchedFormat_NoPacket) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    const std::vector<float> in = {0.5f, 0.5f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    // Wrong sample rate -> ignored.
    enc.FeedFloat32(in.data(), in.size(), 0, frames, 44100, kChannels, out);
    EXPECT_TRUE(out.empty());
    // Wrong channel count -> ignored.
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, 1, out);
    EXPECT_TRUE(out.empty());
}

// ---------------------------------------------------------------------------
// Flush — no buffered state to drain
// ---------------------------------------------------------------------------

TEST(PcmAudioEncoder, Flush_ProducesNoPackets) {
    PcmAudioEncoder enc;
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    const std::vector<float> in(8, 0.25f);
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, kChannels, out);
    out.clear();

    enc.Flush(out);
    EXPECT_TRUE(out.empty());
}

// ---------------------------------------------------------------------------
// ADR 0030: SetBitDepth — 24-bit output
// ---------------------------------------------------------------------------

TEST(PcmAudioEncoder, SetBitDepth24_PacketSizeIs3BytesPerSample) {
    PcmAudioEncoder enc;
    enc.SetBitDepth(24);
    EXPECT_EQ(enc.BitDepth(), 24u);
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    // 2 interleaved samples -> 2 * 3 bytes = 6 bytes
    const std::vector<float> in = {1.0f, -1.0f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, kChannels, out);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].bytes.size(), 6u); // 2 samples * 3 bytes
}

TEST(PcmAudioEncoder, SetBitDepth24_FullScalePositiveLE) {
    // +1.0 -> 8388607 = 0x7FFFFF, LE bytes: FF FF 7F
    PcmAudioEncoder enc;
    enc.SetBitDepth(24);
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, 1, err));

    const std::vector<float> in = {1.0f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, 1, out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].bytes.size(), 3u);
    EXPECT_EQ(out[0].bytes[0], 0xFFu); // LSB
    EXPECT_EQ(out[0].bytes[1], 0xFFu);
    EXPECT_EQ(out[0].bytes[2], 0x7Fu); // MSB
}

TEST(PcmAudioEncoder, SetBitDepth24_FullScaleNegativeLE) {
    // -1.0 -> -8388607 = 0xFF800001 as int32, but stored as 24-bit: 0x800001, LE: 01 00 80
    PcmAudioEncoder enc;
    enc.SetBitDepth(24);
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, 1, err));

    const std::vector<float> in = {-1.0f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, 1, out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].bytes.size(), 3u);
    // -8388607 as uint24 LE: 0x800001 -> bytes 01 00 80
    EXPECT_EQ(out[0].bytes[0], 0x01u);
    EXPECT_EQ(out[0].bytes[1], 0x00u);
    EXPECT_EQ(out[0].bytes[2], 0x80u);
}

// ---------------------------------------------------------------------------
// ADR 0030: SetBitDepth — 32-bit output
// ---------------------------------------------------------------------------

TEST(PcmAudioEncoder, SetBitDepth32_PacketSizeIs4BytesPerSample) {
    PcmAudioEncoder enc;
    enc.SetBitDepth(32);
    EXPECT_EQ(enc.BitDepth(), 32u);
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, kChannels, err));

    // 2 interleaved samples -> 2 * 4 bytes = 8 bytes
    const std::vector<float> in = {1.0f, -1.0f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, kChannels, out);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].bytes.size(), 8u); // 2 samples * 4 bytes
}

TEST(PcmAudioEncoder, SetBitDepth32_FullScalePositiveLE) {
    // +1.0 -> 2147483647 = 0x7FFFFFFF, LE: FF FF FF 7F
    PcmAudioEncoder enc;
    enc.SetBitDepth(32);
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, 1, err));

    const std::vector<float> in = {1.0f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, 1, out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].bytes.size(), 4u);
    EXPECT_EQ(out[0].bytes[0], 0xFFu);
    EXPECT_EQ(out[0].bytes[1], 0xFFu);
    EXPECT_EQ(out[0].bytes[2], 0xFFu);
    EXPECT_EQ(out[0].bytes[3], 0x7Fu);
}

TEST(PcmAudioEncoder, SetBitDepth32_Silence_IsZero) {
    PcmAudioEncoder enc;
    enc.SetBitDepth(32);
    std::string err;
    ASSERT_TRUE(enc.Init(kSampleRate, 1, err));

    const std::vector<float> in = {0.0f};
    uint64_t frames = 0;
    std::vector<EncodedAudioPacket> out;
    enc.FeedFloat32(in.data(), in.size(), 0, frames, kSampleRate, 1, out);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].bytes.size(), 4u);
    for (uint8_t b : out[0].bytes) {
        EXPECT_EQ(b, 0u);
    }
}

// ---------------------------------------------------------------------------
// ADR 0030: SetBitDepth — default is 16-bit (backward-compatible)
// ---------------------------------------------------------------------------

TEST(PcmAudioEncoder, DefaultBitDepth_Is16) {
    PcmAudioEncoder enc;
    EXPECT_EQ(enc.BitDepth(), 16u);
}

TEST(PcmAudioEncoder, SetBitDepth_Invalid_KeepsDefault) {
    PcmAudioEncoder enc;
    enc.SetBitDepth(8); // not accepted
    EXPECT_EQ(enc.BitDepth(), 16u);
    enc.SetBitDepth(0); // not accepted
    EXPECT_EQ(enc.BitDepth(), 16u);
}

// Static helpers: Float32ToS24LE and Float32ToS32
TEST(PcmAudioEncoder, Float32ToS24LE_Silence_IsZero) {
    uint8_t buf[3] = {0xFF, 0xFF, 0xFF};
    PcmAudioEncoder::Float32ToS24LE(0.0f, buf);
    EXPECT_EQ(buf[0], 0u);
    EXPECT_EQ(buf[1], 0u);
    EXPECT_EQ(buf[2], 0u);
}

TEST(PcmAudioEncoder, Float32ToS32_Silence_IsZero) {
    EXPECT_EQ(PcmAudioEncoder::Float32ToS32(0.0f), 0);
}

TEST(PcmAudioEncoder, Float32ToS32_FullScalePositive) {
    EXPECT_EQ(PcmAudioEncoder::Float32ToS32(1.0f), INT32_MAX);
}

TEST(PcmAudioEncoder, Float32ToS32_ClampsAboveOne) {
    EXPECT_EQ(PcmAudioEncoder::Float32ToS32(2.0f), INT32_MAX);
}

} // namespace
