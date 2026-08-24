#include "nvenc_video_encoder.h"
#include <exosnap/engine/interfaces/IVideoEncoder.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace exosnap::engine {

// Compile-time check: NvencVideoEncoder is assignable to IVideoEncoder*
TEST(NvencVideoEncoderInterface, IsAssignableToIVideoEncoder) {
    // This is a compile-time test. If NvencVideoEncoder does not fully implement
    // IVideoEncoder the compilation will fail here. Also exercises the
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

// NvencVideoEncoder::ReapCompleted delegates straight to the underlying
// NvencEncoder, which itself no-ops (returns true, output untouched) when
// not in async mode — the state before InitEncoder has run. Callers can
// therefore call it unconditionally, even before an encode session exists
// (no GPU/NVENC session required for this test).
TEST(NvencVideoEncoderInterface, ReapCompletedNoOpsBeforeAsyncSessionExists) {
    NvencVideoEncoder enc;
    std::vector<EncodedVideoPacket> out;
    std::string err;
    EXPECT_TRUE(enc.ReapCompleted(out, err));
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(err.empty());
}

} // namespace exosnap::engine
