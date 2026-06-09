#include <gtest/gtest.h>

#include "fdk_aac_encoder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using recorder_core::EncodedAudioPacket;
using recorder_core::FdkAacEncoder;

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;

TEST(FdkAacEncoderTest, Init_48kHz_Stereo_Succeeds) {
    FdkAacEncoder encoder;
    std::string err;
    EXPECT_TRUE(encoder.Init(kSampleRate, kChannels, err)) << "Init error: " << err;
    encoder.Shutdown();
}

TEST(FdkAacEncoderTest, Init_CodecPrivate_NotEmpty) {
    FdkAacEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(kSampleRate, kChannels, err)) << "Init error: " << err;

    const auto cp = encoder.CodecPrivateBytes();
    EXPECT_FALSE(cp.empty()) << "CodecPrivateBytes() must not be empty after Init";

    encoder.Shutdown();
}

TEST(FdkAacEncoderTest, FeedFloat32_NullSamples_ProducesAtLeastOnePacket) {
    FdkAacEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(kSampleRate, kChannels, err)) << "Init error: " << err;

    // 2048 float samples (1024 per channel) = exactly 1 AAC-LC frame
    constexpr size_t kTotalSamples = 2048;
    std::vector<float> input(kTotalSamples, 0.0f);

    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated_frames = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, kSampleRate, kChannels, packets);

    EXPECT_FALSE(packets.empty()) << "FeedFloat32 with 2048 null samples must produce at least one packet";
    for (const auto& pkt : packets) {
        EXPECT_FALSE(pkt.bytes.empty()) << "Packet must not have empty byte payload";
    }

    encoder.Shutdown();
}

TEST(FdkAacEncoderTest, Flush_DoesNotCrash) {
    FdkAacEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(kSampleRate, kChannels, err)) << "Init error: " << err;

    // Feed a partial frame (less than 1024 per channel) to exercise the padding path.
    constexpr size_t kPartialSamples = 512; // 256 frames per channel, needs padding
    std::vector<float> input(kPartialSamples, 0.0f);

    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated_frames = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, kSampleRate, kChannels, packets);

    // Flush must not crash regardless of whether it produces output.
    EXPECT_NO_FATAL_FAILURE(encoder.Flush(packets));

    encoder.Shutdown();
}

TEST(FdkAacEncoderTest, Shutdown_IsIdempotent) {
    FdkAacEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(kSampleRate, kChannels, err)) << "Init error: " << err;
    encoder.Shutdown();
    EXPECT_NO_FATAL_FAILURE(encoder.Shutdown());
}

// PTS correctness: packets from FeedFloat32 must be monotonically increasing.
TEST(FdkAacEncoderTest, FeedFloat32_PtsIsMonotonic) {
    FdkAacEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(kSampleRate, kChannels, err)) << "Init error: " << err;

    // Feed 4 full AAC-LC frames (4 × 1024 × 2 = 8192 float samples).
    constexpr size_t kFrames = 4;
    constexpr size_t kTotalSamples = kFrames * 1024 * kChannels;
    std::vector<float> input(kTotalSamples, 0.0f);

    std::vector<EncodedAudioPacket> packets;
    uint64_t accumulated_frames = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, kSampleRate, kChannels, packets);

    ASSERT_GE(packets.size(), 2u) << "Expected at least 2 packets for 4 AAC frames";

    for (size_t i = 1; i < packets.size(); ++i) {
        EXPECT_GT(packets[i].pts_ns, packets[i - 1].pts_ns) << "PTS must be strictly increasing: packet " << i;
    }

    encoder.Shutdown();
}

// PTS correctness: flush packets must follow the last feed packet (not reset to 0).
TEST(FdkAacEncoderTest, FlushPts_ContinuesAfterLastFeedPacket) {
    FdkAacEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(kSampleRate, kChannels, err)) << "Init error: " << err;

    // Feed exactly 2 full frames to get 2 regular packets.
    constexpr size_t kTotalSamples = 2 * 1024 * kChannels;
    std::vector<float> input(kTotalSamples, 0.0f);

    std::vector<EncodedAudioPacket> feed_pkts;
    uint64_t accumulated_frames = 0;
    encoder.FeedFloat32(input.data(), input.size(), 0, accumulated_frames, kSampleRate, kChannels, feed_pkts);
    ASSERT_GE(feed_pkts.size(), 1u) << "Expected at least 1 packet from FeedFloat32";

    const uint64_t last_feed_pts = feed_pkts.back().pts_ns;

    // Feed a partial frame to ensure Flush produces at least one more packet.
    constexpr size_t kPartialSamples = 1024; // 512 per channel — half a frame
    std::vector<float> partial(kPartialSamples, 0.0f);
    std::vector<EncodedAudioPacket> partial_pkts;
    encoder.FeedFloat32(partial.data(), partial.size(), 0, accumulated_frames, kSampleRate, kChannels, partial_pkts);

    std::vector<EncodedAudioPacket> flush_pkts;
    encoder.Flush(flush_pkts);

    if (!flush_pkts.empty()) {
        EXPECT_GT(flush_pkts.front().pts_ns, last_feed_pts)
            << "First flush packet PTS must be greater than last feed packet PTS";
        // All flush packets must be non-zero (not reset to 0).
        for (const auto& pkt : flush_pkts) {
            EXPECT_GT(pkt.pts_ns, 0u) << "Flush packet PTS must not be zero";
        }
    }

    encoder.Shutdown();
}

// PTS correctness: FeedFloat32 with pts_ns=0 and accumulated_frames produces
// absolute timestamps, not double-counted values.
TEST(FdkAacEncoderTest, FeedFloat32_WithZeroPtsBase_PtsMatchesAccumulatedFrames) {
    FdkAacEncoder encoder;
    std::string err;
    ASSERT_TRUE(encoder.Init(kSampleRate, kChannels, err)) << "Init error: " << err;

    // Feed one full frame. Expected PTS = 0 (accumulated_frames=0 at start of call).
    constexpr size_t kOnFrame = 1024 * kChannels;
    std::vector<float> frame(kOnFrame, 0.0f);

    std::vector<EncodedAudioPacket> pkts;
    uint64_t accumulated_frames = 0;
    encoder.FeedFloat32(frame.data(), frame.size(), 0, accumulated_frames, kSampleRate, kChannels, pkts);

    ASSERT_GE(pkts.size(), 1u);
    EXPECT_EQ(pkts[0].pts_ns, 0u) << "First frame at accumulated_frames=0 must have PTS=0";

    // Feed a second frame. Expected PTS = 1024/48000 * 1e9 = 21333333 ns.
    std::vector<EncodedAudioPacket> pkts2;
    encoder.FeedFloat32(frame.data(), frame.size(), 0, accumulated_frames, kSampleRate, kChannels, pkts2);

    ASSERT_GE(pkts2.size(), 1u);
    constexpr uint64_t kExpectedSecondPts = 1024ULL * 1000000000ULL / 48000ULL;
    EXPECT_EQ(pkts2[0].pts_ns, kExpectedSecondPts)
        << "Second frame PTS must be exactly 1024/48000 seconds in nanoseconds";

    encoder.Shutdown();
}

} // namespace
