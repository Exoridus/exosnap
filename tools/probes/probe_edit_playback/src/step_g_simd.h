#pragma once

// Step G: SIMD-vs-scalar variants of exosnap::engine::ConvertFullPlanarYuv420ToBgra
// (libs/engine/src/yuv_to_bgra.cpp), for the 2026-08-01 investigation
// into whether SIMD can shrink the 12.7ms/frame conversion cost measured at
// 2560x1440 8-bit in step C. MEASUREMENT ONLY -- production code untouched;
// these are standalone copies living entirely in this probe.
//
// All four variants (the real baseline + these three) are exercised on the
// SAME dummy frame and compared pixel-for-pixel; see StepG_SimdVariants() in
// main.cpp for the harness and correctness check.

#include "yuv_to_bgra.h"

#include <cstdint>

namespace probe_g {

// 1:1 copy of the scalar 8-bit (YUV420P) loop body from
// exosnap::engine::ConvertFullPlanarYuv420ToBgra, defined in its own
// translation unit (step_g_autovec.cpp) compiled with /arch:AVX2 (see
// CMakeLists.txt) -- answers "does a wider target ISA alone let the MSVC
// auto-vectorizer speed this up, with zero hand-written SIMD?"
void ConvertFullPlanarYuv420ToBgra_AutoVec(const exosnap::engine::FullPlanarYuv420Frame& src,
                                            const exosnap::engine::YuvToBgraParams& params, uint8_t* out_bgra,
                                            uint32_t out_stride_bytes);

// Hand-written SSE2/SSE4.1 intrinsics variant, 8 pixels (4 chroma pairs) per
// iteration. SSE2 is guaranteed present on every x64 CPU -- this is the
// number for the weakest/oldest hardware ExoSnap can run on.
void ConvertFullPlanarYuv420ToBgra_SSE(const exosnap::engine::FullPlanarYuv420Frame& src,
                                        const exosnap::engine::YuvToBgraParams& params, uint8_t* out_bgra,
                                        uint32_t out_stride_bytes);

// Hand-written AVX2 intrinsics variant, 16 pixels (8 chroma pairs) per
// iteration. Defined in step_g_avx2.cpp, compiled with /arch:AVX2. Only
// safe to call if CpuSupportsAvx2() is true.
void ConvertFullPlanarYuv420ToBgra_AVX2(const exosnap::engine::FullPlanarYuv420Frame& src,
                                         const exosnap::engine::YuvToBgraParams& params, uint8_t* out_bgra,
                                         uint32_t out_stride_bytes);

// True if the running CPU reports AVX2 support (CPUID leaf 7, sub-leaf 0,
// EBX bit 5). Implemented in step_g_cpuid.cpp, a TU compiled with NO special
// /arch flag, using only the always-available __cpuid/__cpuidex intrinsics
// -- safe to call unconditionally, on any x64 CPU, before deciding whether
// ConvertFullPlanarYuv420ToBgra_AVX2 may be invoked.
bool CpuSupportsAvx2();

} // namespace probe_g
