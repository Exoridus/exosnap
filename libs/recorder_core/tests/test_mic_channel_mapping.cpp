#include <gtest/gtest.h>

#include "wasapi_capture_src.h"

#include <vector>

namespace {

using recorder_core::MicChannelMode;
using recorder_core::mic_channel_detail::AutoModeState;
using recorder_core::mic_channel_detail::EffectiveAutoMode;
using recorder_core::mic_channel_detail::MapStereoFrameFloat32;
using recorder_core::mic_channel_detail::MapStereoFrameInt16;
using recorder_core::mic_channel_detail::ResolveAutoChannelMode;
using recorder_core::mic_channel_detail::UpdateAutoModeStateFloat32;
using recorder_core::mic_channel_detail::UpdateAutoModeStateInt16;

TEST(MicChannelMappingTest, LeftToStereo_Float32) {
    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(MicChannelMode::LeftToStereo, 0.8f, 0.0f, outL, outR);
    EXPECT_FLOAT_EQ(outL, 0.8f);
    EXPECT_FLOAT_EQ(outR, 0.8f);
}

TEST(MicChannelMappingTest, RightToStereo_Float32) {
    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(MicChannelMode::RightToStereo, 0.0f, 0.7f, outL, outR);
    EXPECT_FLOAT_EQ(outL, 0.7f);
    EXPECT_FLOAT_EQ(outR, 0.7f);
}

TEST(MicChannelMappingTest, MonoMix_Float32) {
    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(MicChannelMode::MonoMix, 1.0f, 0.0f, outL, outR);
    EXPECT_FLOAT_EQ(outL, 0.5f);
    EXPECT_FLOAT_EQ(outR, 0.5f);
}

TEST(MicChannelMappingTest, PreserveStereo_Float32) {
    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(MicChannelMode::PreserveStereo, 0.25f, -0.5f, outL, outR);
    EXPECT_FLOAT_EQ(outL, 0.25f);
    EXPECT_FLOAT_EQ(outR, -0.5f);
}

TEST(MicChannelMappingTest, LeftToStereo_Int16) {
    int16_t outL = 0;
    int16_t outR = 0;
    MapStereoFrameInt16(MicChannelMode::LeftToStereo, 12345, -321, outL, outR);
    EXPECT_EQ(outL, 12345);
    EXPECT_EQ(outR, 12345);
}

TEST(MicChannelMappingTest, AutoResolve_LeftDominant) {
    EXPECT_EQ(ResolveAutoChannelMode(0.05, 0.0001), MicChannelMode::LeftToStereo);
}

TEST(MicChannelMappingTest, AutoResolve_RightDominant) {
    EXPECT_EQ(ResolveAutoChannelMode(0.0001, 0.05), MicChannelMode::RightToStereo);
}

TEST(MicChannelMappingTest, AutoResolve_BalancedStaysPreserve) {
    EXPECT_EQ(ResolveAutoChannelMode(0.03, 0.028), MicChannelMode::PreserveStereo);
}

TEST(MicChannelMappingTest, EffectiveAutoMode_BeforeLock_ReturnsMonoMix) {
    AutoModeState state{};
    EXPECT_FALSE(state.locked);
    EXPECT_EQ(EffectiveAutoMode(state), MicChannelMode::MonoMix);
}

TEST(MicChannelMappingTest, EffectiveAutoMode_AfterLock_ReturnsResolvedMode) {
    AutoModeState state{};
    state.locked = true;
    state.resolved_mode = MicChannelMode::LeftToStereo;
    EXPECT_EQ(EffectiveAutoMode(state), MicChannelMode::LeftToStereo);
}

TEST(MicChannelMappingTest, Auto_BufferLock_LeftOnly_MapsLeftToStereo_Float32) {
    // kAutoDetectMinFrames is now 4800 (100 ms); use that threshold for the 2-step test.
    constexpr uint32_t kFrames = 4800;
    std::vector<float> frames(static_cast<size_t>(kFrames) * 2u, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i) {
        frames[(static_cast<size_t>(i) * 2u) + 0u] = 0.8f;
        frames[(static_cast<size_t>(i) * 2u) + 1u] = 0.0f;
    }

    AutoModeState state{};
    UpdateAutoModeStateFloat32(state, frames.data(), kFrames - 1u, false);
    EXPECT_FALSE(state.locked);
    EXPECT_EQ(EffectiveAutoMode(state), MicChannelMode::MonoMix); // default before lock

    UpdateAutoModeStateFloat32(state, frames.data() + (static_cast<size_t>(kFrames - 1u) * 2u), 1u, false);
    ASSERT_TRUE(state.locked);
    EXPECT_EQ(state.resolved_mode, MicChannelMode::LeftToStereo);

    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(EffectiveAutoMode(state), 0.6f, 0.2f, outL, outR);
    EXPECT_FLOAT_EQ(outL, 0.6f);
    EXPECT_FLOAT_EQ(outR, 0.6f);
}

TEST(MicChannelMappingTest, Auto_BufferLock_RightOnly_MapsRightToStereo_Float32) {
    constexpr uint32_t kFrames = 48000;
    std::vector<float> frames(static_cast<size_t>(kFrames) * 2u, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i) {
        frames[(static_cast<size_t>(i) * 2u) + 0u] = 0.0f;
        frames[(static_cast<size_t>(i) * 2u) + 1u] = 0.75f;
    }

    AutoModeState state{};
    UpdateAutoModeStateFloat32(state, frames.data(), kFrames, false);
    ASSERT_TRUE(state.locked);
    EXPECT_EQ(state.resolved_mode, MicChannelMode::RightToStereo);

    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(EffectiveAutoMode(state), 0.1f, 0.7f, outL, outR);
    EXPECT_FLOAT_EQ(outL, 0.7f);
    EXPECT_FLOAT_EQ(outR, 0.7f);
}

TEST(MicChannelMappingTest, Auto_BufferLock_BalancedStereo_PreservesStereo_Float32) {
    constexpr uint32_t kFrames = 48000;
    std::vector<float> frames(static_cast<size_t>(kFrames) * 2u, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i) {
        frames[(static_cast<size_t>(i) * 2u) + 0u] = 0.30f;
        frames[(static_cast<size_t>(i) * 2u) + 1u] = -0.28f;
    }

    AutoModeState state{};
    UpdateAutoModeStateFloat32(state, frames.data(), kFrames, false);
    ASSERT_TRUE(state.locked);
    EXPECT_EQ(state.resolved_mode, MicChannelMode::PreserveStereo);

    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(EffectiveAutoMode(state), 0.25f, -0.5f, outL, outR);
    EXPECT_FLOAT_EQ(outL, 0.25f);
    EXPECT_FLOAT_EQ(outR, -0.5f);
}

TEST(MicChannelMappingTest, Auto_BufferLock_BothQuiet_ReturnsMonoMix_Float32) {
    constexpr uint32_t kFrames = 48000;
    std::vector<float> frames(static_cast<size_t>(kFrames) * 2u, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i) {
        frames[(static_cast<size_t>(i) * 2u) + 0u] = 0.001f;
        frames[(static_cast<size_t>(i) * 2u) + 1u] = -0.001f;
    }

    AutoModeState state{};
    UpdateAutoModeStateFloat32(state, frames.data(), kFrames, false);
    ASSERT_TRUE(state.locked);
    EXPECT_EQ(state.resolved_mode, MicChannelMode::MonoMix);

    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(EffectiveAutoMode(state), 0.2f, -0.15f, outL, outR);
    EXPECT_FLOAT_EQ(outL, 0.025f);
    EXPECT_FLOAT_EQ(outR, 0.025f);
}

TEST(MicChannelMappingTest, Auto_LeftOnly_WithTinyRightNoise_ReturnsLeftToStereo) {
    // left dominant with tiny noise on right — typical for BEHRINGER-style single-input interface
    EXPECT_EQ(ResolveAutoChannelMode(0.05, 0.0005), MicChannelMode::LeftToStereo);
}

TEST(MicChannelMappingTest, Auto_RightOnly_WithTinyLeftNoise_ReturnsRightToStereo) {
    EXPECT_EQ(ResolveAutoChannelMode(0.0005, 0.05), MicChannelMode::RightToStereo);
}

TEST(MicChannelMappingTest, Auto_BothChannelsSimilar_ReturnsPreserveStereo) {
    EXPECT_EQ(ResolveAutoChannelMode(0.04, 0.038), MicChannelMode::PreserveStereo);
}

TEST(MicChannelMappingTest, Auto_BothVeryQuiet_ReturnsMonoMix) {
    EXPECT_EQ(ResolveAutoChannelMode(0.001, 0.0008), MicChannelMode::MonoMix);
}

TEST(MicChannelMappingTest, Auto_PreLock_EffectiveModeIsMonoMix) {
    AutoModeState state{};
    EXPECT_FALSE(state.locked);
    EXPECT_EQ(EffectiveAutoMode(state), MicChannelMode::MonoMix);
}

TEST(MicChannelMappingTest, Auto_LeftOnly_FinalBufferIsStereoBalanced) {
    // Input: left channel has speech, right channel is silent
    constexpr uint32_t kFrames = 48000;
    std::vector<float> frames(static_cast<size_t>(kFrames) * 2u, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i) {
        frames[(static_cast<size_t>(i) * 2u) + 0u] = 0.6f;
        frames[(static_cast<size_t>(i) * 2u) + 1u] = 0.0f;
    }

    AutoModeState state{};
    UpdateAutoModeStateFloat32(state, frames.data(), kFrames, false);
    ASSERT_TRUE(state.locked);
    ASSERT_EQ(state.resolved_mode, MicChannelMode::LeftToStereo);

    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(EffectiveAutoMode(state), 0.6f, 0.0f, outL, outR);
    EXPECT_FLOAT_EQ(outL, outR);
}

TEST(MicChannelMappingTest, Auto_RightOnly_FinalBufferIsStereoBalanced) {
    constexpr uint32_t kFrames = 48000;
    std::vector<float> frames(static_cast<size_t>(kFrames) * 2u, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i) {
        frames[(static_cast<size_t>(i) * 2u) + 0u] = 0.0f;
        frames[(static_cast<size_t>(i) * 2u) + 1u] = 0.6f;
    }

    AutoModeState state{};
    UpdateAutoModeStateFloat32(state, frames.data(), kFrames, false);
    ASSERT_TRUE(state.locked);
    ASSERT_EQ(state.resolved_mode, MicChannelMode::RightToStereo);

    float outL = 0.0f;
    float outR = 0.0f;
    MapStereoFrameFloat32(EffectiveAutoMode(state), 0.0f, 0.6f, outL, outR);
    EXPECT_FLOAT_EQ(outL, outR);
}

TEST(MicChannelMappingTest, Auto_BufferLock_LeftOnly_MapsLeftToStereo_Int16) {
    constexpr uint32_t kFrames = 48000;
    std::vector<int16_t> frames(static_cast<size_t>(kFrames) * 2u, 0);
    for (uint32_t i = 0; i < kFrames; ++i) {
        frames[(static_cast<size_t>(i) * 2u) + 0u] = 12000;
        frames[(static_cast<size_t>(i) * 2u) + 1u] = 0;
    }

    AutoModeState state{};
    UpdateAutoModeStateInt16(state, frames.data(), kFrames, false);
    ASSERT_TRUE(state.locked);
    EXPECT_EQ(state.resolved_mode, MicChannelMode::LeftToStereo);

    int16_t outL = 0;
    int16_t outR = 0;
    MapStereoFrameInt16(EffectiveAutoMode(state), 12345, -321, outL, outR);
    EXPECT_EQ(outL, 12345);
    EXPECT_EQ(outR, 12345);
}

} // namespace
