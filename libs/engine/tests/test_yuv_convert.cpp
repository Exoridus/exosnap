#include "../src/yuv_convert.h"

#include <gtest/gtest.h>

namespace exosnap::engine {
namespace {

TEST(ConvertI420ToNv12, OutputSizeMatchesNv12Layout) {
    // 4x2 I420: Y=8, U=2, V=2 -> 12 bytes in. NV12 out: Y=8, interleaved UV=4 -> 12 bytes out.
    const uint8_t i420[12] = {1,  2,  3, 4, 5, 6, 7, 8, // Y (4x2)
                              9,  10,                   // U (2x1)
                              11, 12};                  // V (2x1)
    std::vector<uint8_t> nv12;
    ConvertI420ToNv12(i420, 4, 2, nv12);
    ASSERT_EQ(nv12.size(), 12u);
}

TEST(ConvertI420ToNv12, CopiesLumaPlaneUnchanged) {
    const uint8_t i420[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::vector<uint8_t> nv12;
    ConvertI420ToNv12(i420, 4, 2, nv12);
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(nv12[static_cast<size_t>(i)], i420[i]) << "luma byte " << i;
}

TEST(ConvertI420ToNv12, InterleavesUAndVAfterLuma) {
    // Y = 8 bytes of 0. U = {9, 10}. V = {11, 12}.
    const uint8_t i420[12] = {0, 0, 0, 0, 0, 0, 0, 0, 9, 10, 11, 12};
    std::vector<uint8_t> nv12;
    ConvertI420ToNv12(i420, 4, 2, nv12);
    // NV12 UV plane starts at offset 8: U0 V0 U1 V1 = 9 11 10 12.
    EXPECT_EQ(nv12[8], 9);
    EXPECT_EQ(nv12[9], 11);
    EXPECT_EQ(nv12[10], 10);
    EXPECT_EQ(nv12[11], 12);
}

TEST(ConvertI420ToNv12, HandlesLargerEvenDimensions) {
    // 8x4: Y=32, U=8 (4x2), V=8 (4x2) -> 48 bytes in; NV12 out = 32 + 16 = 48.
    std::vector<uint8_t> i420(48);
    for (size_t i = 0; i < i420.size(); ++i)
        i420[i] = static_cast<uint8_t>(i);
    std::vector<uint8_t> nv12;
    ConvertI420ToNv12(i420.data(), 8, 4, nv12);
    ASSERT_EQ(nv12.size(), 48u);
    // Luma unchanged.
    for (int i = 0; i < 32; ++i)
        EXPECT_EQ(nv12[static_cast<size_t>(i)], i420[static_cast<size_t>(i)]);
    // First interleaved pair: U plane starts at i420[32], V plane at i420[40].
    EXPECT_EQ(nv12[32], i420[32]); // U0
    EXPECT_EQ(nv12[33], i420[40]); // V0
    EXPECT_EQ(nv12[34], i420[33]); // U1
    EXPECT_EQ(nv12[35], i420[41]); // V1
}

} // namespace
} // namespace exosnap::engine
