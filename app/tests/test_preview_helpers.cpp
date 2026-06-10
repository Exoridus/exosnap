#include <gtest/gtest.h>

#include "../services/PreviewHelpers.h"

#include <Windows.h>

namespace exosnap {
namespace {

TEST(PreviewFpsHelpers, Standard60FpsReturns16ms) {
    EXPECT_EQ(PreviewFrameIntervalMs(60, 1), 16u);
}

TEST(PreviewFpsHelpers, Standard30FpsReturns33ms) {
    EXPECT_EQ(PreviewFrameIntervalMs(30, 1), 33u);
}

TEST(PreviewFpsHelpers, Standard24FpsReturns41ms) {
    EXPECT_EQ(PreviewFrameIntervalMs(24, 1), 41u);
}

TEST(PreviewFpsHelpers, NumZeroReturnsDefault16ms) {
    EXPECT_EQ(PreviewFrameIntervalMs(0, 1), 16u);
}

TEST(PreviewFpsHelpers, DenZeroReturnsDefault16ms) {
    EXPECT_EQ(PreviewFrameIntervalMs(60, 0), 16u);
}

TEST(PreviewFpsHelpers, BothZeroReturnsDefault16ms) {
    EXPECT_EQ(PreviewFrameIntervalMs(0, 0), 16u);
}

TEST(PreviewFpsHelpers, FractionalFpsReturnsCorrectMs) {
    EXPECT_EQ(PreviewFrameIntervalMs(60000, 1001), 16u);
    EXPECT_EQ(PreviewFrameIntervalMs(30000, 1001), 33u);
}

TEST(PreviewGeometry, ExactFit1920x1080Into1920x1080) {
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(1920, 1080, 1920, 1080, x, y, w, h);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(w, 1920);
    EXPECT_EQ(h, 1080);
}

TEST(PreviewGeometry, ScaleDownIntoSmallerTarget) {
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(640, 360, 1920, 1080, x, y, w, h);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(w, 640);
    EXPECT_EQ(h, 360);
}

TEST(PreviewGeometry, PillarboxForVerticalSource) {
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(1920, 1080, 1080, 1920, x, y, w, h);
    EXPECT_GT(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_LT(w, 1920);
    EXPECT_EQ(h, 1080);
}

TEST(PreviewGeometry, LetterboxForWideSource) {
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(1920, 1080, 3440, 1440, x, y, w, h);
    EXPECT_EQ(x, 0);
    EXPECT_GT(y, 0);
    EXPECT_EQ(w, 1920);
    EXPECT_LT(h, 1080);
}

TEST(PreviewGeometry, ZeroSourceFillsTarget) {
    LONG x = -1, y = -1, w = -1, h = -1;
    ComputeContainFitRect(1920, 1080, 0, 1080, x, y, w, h);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
    EXPECT_EQ(w, 1920);
    EXPECT_EQ(h, 1080);
}

} // namespace
} // namespace exosnap
