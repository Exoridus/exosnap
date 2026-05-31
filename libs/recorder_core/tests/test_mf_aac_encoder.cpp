#include <gtest/gtest.h>

#include "mf_aac_encoder.h"

#include <cstdint>
#include <string>
#include <vector>

#include <objbase.h>

namespace {

using recorder_core::EncodedAudioPacket;
using recorder_core::MfAacEncoder;

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;
// AAC-LC encodes 1024 PCM frames per output packet.
constexpr uint32_t kAacFrameSamples = 1024;
constexpr uint64_t kAacFrameDurNs = static_cast<uint64_t>(kAacFrameSamples) * 1000000000ULL / kSampleRate; // 21333333

// RAII helper: the MF AAC MFT is an in-proc COM object, so COM must be
// initialised on the calling thread (the AudioThread does this in production).
class ComScope {
  public:
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        inited_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        owns_ = SUCCEEDED(hr) && hr != RPC_E_CHANGED_MODE;
    }
    ~ComScope() {
        if (owns_)
            CoUninitialize();
    }
    bool ok() const {
        return inited_;
    }

  private:
    bool inited_ = false;
    bool owns_ = false;
};

TEST(MfAacEncoderTest, MfAacEncoder_PtsStepMatchesAacFrameDuration) {
    ComScope com;
    ASSERT_TRUE(com.ok());

    MfAacEncoder encoder;
    std::string err;
    if (!encoder.Init(kSampleRate, kChannels, err)) {
        GTEST_SKIP() << "MF AAC encoder unavailable on this host: " << err;
    }

    // Feed audio in 480-frame (10 ms) chunks with strictly sample-count-derived
    // PTS, exactly as audio_thread.cpp does after the QPC idle path was removed.
    // 200 chunks * 480 frames = 96000 frames (~2 s) -> ~93 AAC packets.
    uint64_t framesDelivered = 0;
    uint64_t accumulatedFrames = 0;
    std::vector<EncodedAudioPacket> packets;
    std::vector<float> chunk(static_cast<size_t>(480) * kChannels, 0.0f);

    for (int i = 0; i < 200; ++i) {
        const uint64_t pts_ns = framesDelivered * 1000000000ULL / kSampleRate;
        encoder.FeedFloat32(chunk.data(), chunk.size(), pts_ns, accumulatedFrames, kSampleRate, kChannels, packets);
        framesDelivered += 480;
    }
    encoder.Flush(packets);
    encoder.Shutdown();

    if (packets.size() < 5) {
        GTEST_SKIP() << "MF AAC encoder produced too few packets (" << packets.size() << ") to assess timing";
    }

    // Monotonic, non-decreasing PTS.
    for (size_t i = 1; i < packets.size(); ++i) {
        EXPECT_GE(packets[i].pts_ns, packets[i - 1].pts_ns) << "AAC PTS must not go backwards at packet " << i;
    }

    // Average inter-packet step must match the AAC frame duration. Skip the very
    // first packet to stay robust against encoder priming/delay on packet 0.
    const uint64_t span = packets.back().pts_ns - packets[1].pts_ns;
    const double avgStep = static_cast<double>(span) / static_cast<double>(packets.size() - 2);

    EXPECT_GE(avgStep, kAacFrameDurNs * 0.95)
        << "Average AAC PTS step " << avgStep << " ns collapsed below the frame duration";
    EXPECT_LE(avgStep, kAacFrameDurNs * 1.05)
        << "Average AAC PTS step " << avgStep << " ns is inflated above the frame duration";
}

} // namespace
