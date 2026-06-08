#include <gtest/gtest.h>

#include "opus_audio_encoder.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#if __has_include(<opus.h>)
#include <opus.h>
#else
extern "C" {
struct OpusDecoder;
using opus_int32 = int;
OpusDecoder* opus_decoder_create(opus_int32 Fs, int channels, int* error);
void opus_decoder_destroy(OpusDecoder* st);
int opus_decode_float(OpusDecoder* st, const unsigned char* data, opus_int32 len, float* pcm, int frame_size,
                      int decode_fec);
}
constexpr int OPUS_OK = 0;
#endif

namespace {

using recorder_core::EncodedAudioPacket;
using recorder_core::OpusAudioEncoder;

TEST(OpusEncoderTest, OpusEncoder_InitSucceeds) {
    OpusAudioEncoder encoder;
    std::string err;
    EXPECT_TRUE(encoder.Init(48000, 2, err));
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_InitRejectsZeroSampleRate) {
    OpusAudioEncoder encoder;
    std::string err;
    EXPECT_FALSE(encoder.Init(0, 2, err));
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_InitRejectsZeroChannels) {
    OpusAudioEncoder encoder;
    std::string err;
    EXPECT_FALSE(encoder.Init(48000, 0, err));
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_FeedFloat32_ProducesPackets) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));

    std::vector<float> input(1920, 0.0f);
    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated_frames = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, 48000, 2, packets);

    ASSERT_FALSE(packets.empty());
    EXPECT_GT(packets.front().bytes.size(), 0u);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_FeedFloat32_AccumulatesShortChunks) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));

    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated_frames = 0;

    std::vector<float> chunk_a(960, 0.0f);
    encoder.FeedFloat32(chunk_a.data(), chunk_a.size(), 0, accumulated_frames, 48000, 2, packets);
    EXPECT_TRUE(packets.empty());

    std::vector<float> chunk_b(960, 0.0f);
    encoder.FeedFloat32(chunk_b.data(), chunk_b.size(), 0, accumulated_frames, 48000, 2, packets);
    EXPECT_FALSE(packets.empty());
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_Flush_ProducesFinalPacket) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));

    std::vector<float> input(500, 0.0f);
    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated_frames = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, 48000, 2, packets);
    EXPECT_TRUE(packets.empty());

    encoder.Flush(packets);
    ASSERT_FALSE(packets.empty());
    EXPECT_GT(packets.back().bytes.size(), 0u);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_CodecPrivateBytes_Has19Bytes) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));
    const auto bytes = encoder.CodecPrivateBytes();
    EXPECT_EQ(bytes.size(), 19u);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_CodecPrivateBytes_MagicIsOpusHead) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));
    const auto bytes = encoder.CodecPrivateBytes();
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], static_cast<uint8_t>('O'));
    EXPECT_EQ(bytes[1], static_cast<uint8_t>('p'));
    EXPECT_EQ(bytes[2], static_cast<uint8_t>('u'));
    EXPECT_EQ(bytes[3], static_cast<uint8_t>('s'));
    EXPECT_EQ(bytes[4], static_cast<uint8_t>('H'));
    EXPECT_EQ(bytes[5], static_cast<uint8_t>('e'));
    EXPECT_EQ(bytes[6], static_cast<uint8_t>('a'));
    EXPECT_EQ(bytes[7], static_cast<uint8_t>('d'));
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_CodecPrivateBytes_VersionIs1) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));
    const auto bytes = encoder.CodecPrivateBytes();
    ASSERT_GE(bytes.size(), 9u);
    EXPECT_EQ(bytes[8], 0x01);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_CodecPrivateBytes_Channels2) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));
    const auto bytes = encoder.CodecPrivateBytes();
    ASSERT_GE(bytes.size(), 10u);
    EXPECT_EQ(bytes[9], 0x02);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_CodecPrivateBytes_SampleRateIs48k) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));
    const auto bytes = encoder.CodecPrivateBytes();
    ASSERT_GE(bytes.size(), 16u);
    EXPECT_EQ(bytes[12], 0x80);
    EXPECT_EQ(bytes[13], 0xBB);
    EXPECT_EQ(bytes[14], 0x00);
    EXPECT_EQ(bytes[15], 0x00);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_CodecPrivateBytes_MappingFamilyZero) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));
    const auto bytes = encoder.CodecPrivateBytes();
    ASSERT_GE(bytes.size(), 19u);
    EXPECT_EQ(bytes[18], 0x00);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_CodecPrivateBytes_PreSkipIsPositive) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));
    const auto bytes = encoder.CodecPrivateBytes();
    ASSERT_GE(bytes.size(), 19u);
    ASSERT_EQ(bytes[0], static_cast<uint8_t>('O'));
    ASSERT_EQ(bytes[1], static_cast<uint8_t>('p'));
    ASSERT_EQ(bytes[2], static_cast<uint8_t>('u'));
    ASSERT_EQ(bytes[3], static_cast<uint8_t>('s'));
    ASSERT_EQ(bytes[4], static_cast<uint8_t>('H'));
    ASSERT_EQ(bytes[5], static_cast<uint8_t>('e'));
    ASSERT_EQ(bytes[6], static_cast<uint8_t>('a'));
    ASSERT_EQ(bytes[7], static_cast<uint8_t>('d'));
    const uint16_t pre_skip = static_cast<uint16_t>(bytes[10]) | (static_cast<uint16_t>(bytes[11]) << 8u);
    EXPECT_GT(pre_skip, 0u);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_PtsMonotonicallyIncreasing) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));

    std::vector<float> input(3840, 0.0f);
    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated_frames = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, 48000, 2, packets);

    ASSERT_GE(packets.size(), 2u);
    EXPECT_LT(packets[0].pts_ns, packets[1].pts_ns);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_EncodedPacketCanBeDecoded) {
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;
    constexpr size_t kFrameSamplesPerChannel = 960;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kFreqHz = 440.0;

    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(kSampleRate, kChannels, err));

    std::vector<float> input(kFrameSamplesPerChannel * kChannels, 0.0f);
    for (size_t i = 0; i < kFrameSamplesPerChannel; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
        const float l = static_cast<float>(0.2 * std::sin(2.0 * kPi * kFreqHz * t));
        const float r = static_cast<float>(0.15 * std::sin(2.0 * kPi * (kFreqHz * 1.5) * t));
        input[i * 2] = l;
        input[i * 2 + 1] = r;
    }

    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated_frames = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, kSampleRate, kChannels, packets);

    const EncodedAudioPacket* first_packet = nullptr;
    for (const auto& packet : packets) {
        if (!packet.bytes.empty()) {
            first_packet = &packet;
            break;
        }
    }
    ASSERT_NE(first_packet, nullptr);

    int dec_err = OPUS_OK;
    OpusDecoder* decoder =
        opus_decoder_create(static_cast<opus_int32>(kSampleRate), static_cast<int>(kChannels), &dec_err);
    ASSERT_NE(decoder, nullptr);
    ASSERT_EQ(dec_err, OPUS_OK);

    std::vector<float> decoded(kFrameSamplesPerChannel * kChannels, 0.0f);
    const int decoded_frames =
        opus_decode_float(decoder, first_packet->bytes.data(), static_cast<opus_int32>(first_packet->bytes.size()),
                          decoded.data(), static_cast<int>(kFrameSamplesPerChannel), 0);

    EXPECT_GT(decoded_frames, 0);
    EXPECT_LE(decoded_frames, static_cast<int>(kFrameSamplesPerChannel));

    const size_t decoded_sample_count = static_cast<size_t>(decoded_frames) * kChannels;
    double energy = 0.0;
    for (size_t i = 0; i < decoded_sample_count; ++i) {
        EXPECT_TRUE(std::isfinite(decoded[i]));
        energy += static_cast<double>(decoded[i]) * static_cast<double>(decoded[i]);
    }
    EXPECT_GT(energy, 1e-8);

    opus_decoder_destroy(decoder);
    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_PtsStepIs20ms_PerFrame) {
    // 960 samples @ 48 kHz = exactly 20 ms per Opus frame.
    // Feeding data in 480-sample chunks (simulating 10 ms WASAPI delivery)
    // must still produce strictly 20 ms PTS steps.
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));

    uint64_t accumulated_frames = 0;
    std::vector<EncodedAudioPacket> packets;

    // 10 chunks × 480 frames/chunk × 2 ch = 9600 floats → 5 Opus packets
    std::vector<float> chunk(480 * 2, 0.0f);
    for (int i = 0; i < 10; ++i) {
        encoder.FeedFloat32(chunk.data(), chunk.size(), 0, accumulated_frames, 48000, 2, packets);
    }

    ASSERT_EQ(packets.size(), 5u);
    constexpr uint64_t kStep = 20000000ULL; // 20 ms in ns
    EXPECT_EQ(packets[0].pts_ns, 0 * kStep);
    EXPECT_EQ(packets[1].pts_ns, 1 * kStep);
    EXPECT_EQ(packets[2].pts_ns, 2 * kStep);
    EXPECT_EQ(packets[3].pts_ns, 3 * kStep);
    EXPECT_EQ(packets[4].pts_ns, 4 * kStep);

    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_PtsStepIs20ms_NOpusFrames) {
    // N consecutive Opus packets must have PTS = N * 20 ms.
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));

    uint64_t accumulated_frames = 0;
    std::vector<EncodedAudioPacket> packets;

    // Feed 8 complete Opus frames in one shot
    std::vector<float> input(8 * 960 * 2, 0.0f);
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, 48000, 2, packets);

    ASSERT_EQ(packets.size(), 8u);
    constexpr uint64_t kStep = 20000000ULL;
    for (size_t i = 0; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].pts_ns, i * kStep) << "Packet " << i << " has wrong PTS";
    }

    encoder.Shutdown();
}

TEST(OpusEncoderTest, OpusEncoder_Shutdown_IsIdempotent) {
    OpusAudioEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(48000, 2, err));
    encoder.Shutdown();
    encoder.Shutdown();
    SUCCEED();
}

} // namespace
