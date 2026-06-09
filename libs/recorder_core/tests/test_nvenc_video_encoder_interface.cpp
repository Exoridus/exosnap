#include "nvenc_video_encoder.h"
#include <recorder_core/interfaces/IVideoEncoder.h>

#include <gtest/gtest.h>

#include <memory>

namespace recorder_core {

// Compile-time check: NvencVideoEncoder is assignable to IVideoEncoder*
TEST(NvencVideoEncoderInterface, IsAssignableToIVideoEncoder) {
    // This is a compile-time test. If NvencVideoEncoder does not fully implement
    // IVideoEncoder the compilation will fail here.
    std::unique_ptr<IVideoEncoder> enc = std::make_unique<NvencVideoEncoder>();
    EXPECT_NE(enc.get(), nullptr);
}

TEST(NvencVideoEncoderInterface, SlotCountReturns8) {
    NvencVideoEncoder enc;
    EXPECT_EQ(enc.SlotCount(), 8);
}

} // namespace recorder_core
