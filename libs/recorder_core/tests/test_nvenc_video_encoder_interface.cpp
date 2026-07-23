#include "nvenc_video_encoder.h"
#include <recorder_core/interfaces/IVideoEncoder.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace recorder_core {

// Compile-time check: NvencVideoEncoder is assignable to IVideoEncoder*
TEST(NvencVideoEncoderInterface, IsAssignableToIVideoEncoder) {
    // This is a compile-time test. If NvencVideoEncoder does not fully implement
    // IVideoEncoder the compilation will fail here. Also exercises the S7
    // vector-based EncodeFrame signature at compile time: NvencVideoEncoder must
    // implement `EncodeFrame(..., std::vector<EncodedVideoPacket>&, std::string&)`
    // to satisfy the pure-virtual interface.
    std::unique_ptr<IVideoEncoder> enc = std::make_unique<NvencVideoEncoder>();
    EXPECT_NE(enc.get(), nullptr);
}

TEST(NvencVideoEncoderInterface, SlotCountReturns8) {
    NvencVideoEncoder enc;
    EXPECT_EQ(enc.SlotCount(), 8);
}

// S7: NvencVideoEncoder does not yet override ReapCompleted (S8 adds the async
// implementation) - IVideoEncoder's default no-op must report success and
// leave the output untouched, so callers can call it unconditionally even
// before an encode session exists (no GPU/NVENC session required for this).
TEST(NvencVideoEncoderInterface, ReapCompletedDefaultsToNoOpSuccess) {
    NvencVideoEncoder enc;
    std::vector<EncodedVideoPacket> out;
    std::string err;
    EXPECT_TRUE(enc.ReapCompleted(out, err));
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(err.empty());
}

} // namespace recorder_core
