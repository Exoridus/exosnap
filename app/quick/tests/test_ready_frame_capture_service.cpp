#include "ReadyFrameCaptureService.h"

#include <gtest/gtest.h>

namespace exosnap::quick {
namespace {

TEST(ReadyFrameCaptureServiceTest, FullSourceMapsToFullPixelExtent) {
    EXPECT_EQ(ReadyFrameCaptureService::SourceCropPixels(1920, 1080, QRectF(0.0, 0.0, 1.0, 1.0)),
              QRect(0, 0, 1920, 1080));
}

TEST(ReadyFrameCaptureServiceTest, NormalizedRegionUsesOutwardPixelRounding) {
    EXPECT_EQ(ReadyFrameCaptureService::SourceCropPixels(100, 80, QRectF(0.101, 0.201, 0.302, 0.404)),
              QRect(10, 16, 31, 33));
}

TEST(ReadyFrameCaptureServiceTest, DegenerateAndOutOfBoundsRegionsRemainSafe) {
    EXPECT_EQ(ReadyFrameCaptureService::SourceCropPixels(0, 1080, QRectF(0.0, 0.0, 1.0, 1.0)), QRect());
    EXPECT_EQ(ReadyFrameCaptureService::SourceCropPixels(64, 64, QRectF(2.0, 2.0, -3.0, -3.0)), QRect(0, 0, 64, 64));
    EXPECT_EQ(ReadyFrameCaptureService::SourceCropPixels(64, 64, QRectF(1.0, 1.0, 0.0, 0.0)), QRect(63, 63, 1, 1));
}

} // namespace
} // namespace exosnap::quick
