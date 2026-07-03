#include <gtest/gtest.h>

#include "yuv_to_bgra.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

// Micro-benchmark for the NV12/P010 -> BGRA conversion that runs
// synchronously on VideoThread at the ~30 Hz preview cadence (Strand 3
// slice 1). Headless: synthetic buffers, no D3D11/GPU. Prints per-frame
// timings for 1080p and 4K; intentionally asserts NOTHING about time so it
// can never flake CI -- the numbers are for the engineering record (see the
// slice deliverable). Correctness is asserted via a checksum so the
// optimizer cannot elide the conversion.

namespace {

using recorder_core::ColorRange;
using recorder_core::ConvertYuv420ToBgra;
using recorder_core::MatrixCoefficients;
using recorder_core::PlanarYuv420Frame;
using recorder_core::YuvToBgraParams;

double BenchNv12(uint32_t width, uint32_t height, int iterations) {
    std::vector<uint8_t> y_plane(static_cast<size_t>(width) * height);
    std::vector<uint8_t> uv_plane(static_cast<size_t>(width) * height / 2);
    for (size_t i = 0; i < y_plane.size(); ++i)
        y_plane[i] = static_cast<uint8_t>(i * 7u);
    for (size_t i = 0; i < uv_plane.size(); ++i)
        uv_plane[i] = static_cast<uint8_t>(64u + i * 3u);

    PlanarYuv420Frame src;
    src.y_plane = y_plane.data();
    src.y_stride_bytes = width;
    src.uv_plane = uv_plane.data();
    src.uv_stride_bytes = width;
    src.width = width;
    src.height = height;
    src.bits_per_sample = 8;

    YuvToBgraParams params;
    params.matrix = MatrixCoefficients::Bt709;
    params.range = ColorRange::Full;

    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);

    // Warm-up (page in buffers, prime caches)
    ConvertYuv420ToBgra(src, params, out.data(), width * 4);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        ConvertYuv420ToBgra(src, params, out.data(), width * 4);
    }
    const auto t1 = std::chrono::steady_clock::now();

    // Keep the result observable so the loop cannot be optimized away.
    // Odd stride rotates through all four channels (alpha included, which is
    // always 255) so the checksum is guaranteed non-zero.
    uint64_t checksum = 0;
    for (size_t i = 0; i < out.size(); i += 1021)
        checksum += out[i];
    EXPECT_GT(checksum, 0u);

    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iterations;
}

} // namespace

TEST(YuvToBgraBench, PerFrameConversionCost) {
    const double ms_1080p = BenchNv12(1920, 1080, 30);
    const double ms_4k = BenchNv12(3840, 2160, 15);

    std::cout << "[bench] NV12->BGRA 1920x1080: " << ms_1080p << " ms/frame\n";
    std::cout << "[bench] NV12->BGRA 3840x2160: " << ms_4k << " ms/frame\n";
    ::testing::Test::RecordProperty("ms_per_frame_1080p", std::to_string(ms_1080p));
    ::testing::Test::RecordProperty("ms_per_frame_4k", std::to_string(ms_4k));
    // No timing assertions: numbers vary wildly across CI runners and Debug
    // vs Release. The printout is the deliverable.
    SUCCEED();
}
