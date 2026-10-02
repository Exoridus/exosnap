#include "exosnap/engine/wgc_frame_geometry.h"

#include <gtest/gtest.h>

using exosnap::engine::CaptureContentSize;
using exosnap::engine::WgcFrameGeometry;
using exosnap::engine::WgcFrameGeometryTracker;

namespace {

constexpr CaptureContentSize kSession{1282, 752};
constexpr CaptureContentSize kIconic{146, 28};

TEST(WgcFrameGeometry, SessionSizedFrameIsEncoded) {
    WgcFrameGeometryTracker tracker(kSession);
    EXPECT_EQ(tracker.Classify(kSession, /*window_minimized=*/false), WgcFrameGeometry::Encode);
}

// A minimized window keeps delivering frames at its iconic caption size. That
// is not a resize of the recorded source: the recording keeps its last frame.
TEST(WgcFrameGeometry, MinimizedWindowHoldsTheLastFrame) {
    WgcFrameGeometryTracker tracker(kSession);
    ASSERT_EQ(tracker.Classify(kSession, false), WgcFrameGeometry::Encode);
    EXPECT_EQ(tracker.Classify(kIconic, /*window_minimized=*/true), WgcFrameGeometry::HoldLastFrame);
    EXPECT_EQ(tracker.Classify(kIconic, true), WgcFrameGeometry::HoldLastFrame);
}

TEST(WgcFrameGeometry, RestoredWindowEncodesAgain) {
    WgcFrameGeometryTracker tracker(kSession);
    ASSERT_EQ(tracker.Classify(kSession, false), WgcFrameGeometry::Encode);
    ASSERT_EQ(tracker.Classify(kIconic, true), WgcFrameGeometry::HoldLastFrame);
    EXPECT_EQ(tracker.Classify(kSession, false), WgcFrameGeometry::Encode);
}

// A frame rendered while minimized can still be queued when the window is
// already restored. Its iconic size identifies it as stale, not as a resize.
TEST(WgcFrameGeometry, StaleIconicFrameAfterRestoreIsHeld) {
    WgcFrameGeometryTracker tracker(kSession);
    ASSERT_EQ(tracker.Classify(kSession, false), WgcFrameGeometry::Encode);
    ASSERT_EQ(tracker.Classify(kIconic, true), WgcFrameGeometry::HoldLastFrame);
    EXPECT_EQ(tracker.Classify(kIconic, /*window_minimized=*/false), WgcFrameGeometry::HoldLastFrame);
}

// The stale-frame allowance ends with the minimized episode. Once the source
// delivered its recorded size again, the old iconic size is an ordinary resize.
TEST(WgcFrameGeometry, IconicAllowanceEndsWithTheNextEncodedFrame) {
    WgcFrameGeometryTracker tracker(kSession);
    ASSERT_EQ(tracker.Classify(kSession, false), WgcFrameGeometry::Encode);
    ASSERT_EQ(tracker.Classify(kIconic, true), WgcFrameGeometry::HoldLastFrame);
    ASSERT_EQ(tracker.Classify(kSession, false), WgcFrameGeometry::Encode);
    EXPECT_EQ(tracker.Classify(kIconic, false), WgcFrameGeometry::SourceResized);
}

TEST(WgcFrameGeometry, VisibleResizeEndsTheRecording) {
    WgcFrameGeometryTracker tracker(kSession);
    ASSERT_EQ(tracker.Classify(kSession, false), WgcFrameGeometry::Encode);
    EXPECT_EQ(tracker.Classify({1600, 900}, false), WgcFrameGeometry::SourceResized);
}

// Restoring a minimized window to a different size is a real resize, not a
// stale frame: holding would silently record a frozen image of a live window.
TEST(WgcFrameGeometry, RestoreToADifferentSizeEndsTheRecording) {
    WgcFrameGeometryTracker tracker(kSession);
    ASSERT_EQ(tracker.Classify(kSession, false), WgcFrameGeometry::Encode);
    ASSERT_EQ(tracker.Classify(kIconic, true), WgcFrameGeometry::HoldLastFrame);
    EXPECT_EQ(tracker.Classify({1600, 900}, false), WgcFrameGeometry::SourceResized);
}

// Holding needs a frame to hold. Without one the session could only run on
// with no video, so a mismatch before the first encoded frame stays a failure.
TEST(WgcFrameGeometry, MinimizedBeforeAnyEncodedFrameIsAResize) {
    WgcFrameGeometryTracker tracker(kSession);
    EXPECT_EQ(tracker.Classify(kIconic, true), WgcFrameGeometry::SourceResized);
}

} // namespace
