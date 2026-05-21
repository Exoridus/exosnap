#include <gtest/gtest.h>

#include <recorder_core/audio_meter.h>

#include <cmath>
#include <cstdint>
#include <iterator>

namespace {

using recorder_core::ComputeRmsFromInt16;
using recorder_core::ComputeRmsLinear;
using recorder_core::kDbFloor;
using recorder_core::RmsToDbfs;
using recorder_core::RmsToMeter01;

TEST(AudioMeterTest, ComputeRmsLinear_NullPointer_ReturnsZero) {
    EXPECT_FLOAT_EQ(ComputeRmsLinear(nullptr, 4), 0.0f);
}

TEST(AudioMeterTest, ComputeRmsLinear_Empty_ReturnsZero) {
    const float samples[] = {1.0f, 2.0f};
    EXPECT_FLOAT_EQ(ComputeRmsLinear(samples, 0), 0.0f);
}

TEST(AudioMeterTest, ComputeRmsLinear_AllZeros_ReturnsZero) {
    const float samples[] = {0.0f, 0.0f, 0.0f, 0.0f};
    EXPECT_FLOAT_EQ(ComputeRmsLinear(samples, std::size(samples)), 0.0f);
}

TEST(AudioMeterTest, ComputeRmsLinear_AllOnes_ReturnsOne) {
    const float samples[] = {1.0f, 1.0f, 1.0f, 1.0f};
    EXPECT_FLOAT_EQ(ComputeRmsLinear(samples, std::size(samples)), 1.0f);
}

TEST(AudioMeterTest, ComputeRmsLinear_AlternatingPlusMinusOnes_ReturnsOne) {
    const float samples[] = {1.0f, -1.0f, 1.0f, -1.0f};
    EXPECT_FLOAT_EQ(ComputeRmsLinear(samples, std::size(samples)), 1.0f);
}

TEST(AudioMeterTest, ComputeRmsLinear_HalfAmplitude_ReturnsHalf) {
    const float samples[] = {0.5f, -0.5f, 0.5f, -0.5f};
    EXPECT_FLOAT_EQ(ComputeRmsLinear(samples, std::size(samples)), 0.5f);
}

TEST(AudioMeterTest, ComputeRmsFromInt16_AllZeros_ReturnsZero) {
    const std::int16_t samples[] = {0, 0, 0, 0};
    EXPECT_FLOAT_EQ(ComputeRmsFromInt16(samples, std::size(samples)), 0.0f);
}

TEST(AudioMeterTest, ComputeRmsFromInt16_AllMaxPositive_ReturnsNearOne) {
    const std::int16_t samples[] = {32767, 32767, 32767, 32767};
    EXPECT_NEAR(ComputeRmsFromInt16(samples, std::size(samples)), 1.0f, 0.001f);
}

TEST(AudioMeterTest, ComputeRmsFromInt16_AlternatingMaxPositiveNegative_ReturnsNearOne) {
    const std::int16_t samples[] = {32767, -32768, 32767, -32768};
    EXPECT_NEAR(ComputeRmsFromInt16(samples, std::size(samples)), 1.0f, 0.001f);
}

TEST(AudioMeterTest, RmsToDbfs_One_ReturnsZero) {
    EXPECT_FLOAT_EQ(RmsToDbfs(1.0f), 0.0f);
}

TEST(AudioMeterTest, RmsToDbfs_Zero_ReturnsFloor) {
    EXPECT_FLOAT_EQ(RmsToDbfs(0.0f), kDbFloor);
}

TEST(AudioMeterTest, RmsToDbfs_Point316_ReturnsAboutMinusTen) {
    EXPECT_NEAR(RmsToDbfs(0.31622776f), -10.0f, 0.1f);
}

TEST(AudioMeterTest, RmsToMeter01_One_ReturnsOne) {
    EXPECT_FLOAT_EQ(RmsToMeter01(1.0f), 1.0f);
}

TEST(AudioMeterTest, RmsToMeter01_Zero_ReturnsZero) {
    EXPECT_FLOAT_EQ(RmsToMeter01(0.0f), 0.0f);
}

TEST(AudioMeterTest, RmsToMeter01_Minus30Dbfs_ReturnsHalf) {
    const float rms_linear = std::pow(10.0f, -30.0f / 20.0f);
    EXPECT_NEAR(RmsToMeter01(rms_linear), 0.5f, 0.01f);
}

} // namespace
