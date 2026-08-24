#include "exosnap/engine/od_acquire_classify.h"
#include <gtest/gtest.h>

using namespace exosnap::engine;

TEST(ClassifyOdAcquire, PresentIsAlwaysDesktopPresent) {
    EXPECT_EQ(ClassifyOdAcquire(/*has_present=*/true, /*has_mouse_update=*/false, /*cursor_capture_enabled=*/false),
              OdAcquireKind::DesktopPresent);
    EXPECT_EQ(ClassifyOdAcquire(true, true, true), OdAcquireKind::DesktopPresent);
}

TEST(ClassifyOdAcquire, MouseOnlyWithCursorCaptureOnIsCursorOnly) {
    EXPECT_EQ(ClassifyOdAcquire(/*has_present=*/false, /*has_mouse_update=*/true, /*cursor_capture_enabled=*/true),
              OdAcquireKind::CursorOnly);
}

TEST(ClassifyOdAcquire, MouseOnlyWithCursorCaptureOffIsIgnorable) {
    EXPECT_EQ(ClassifyOdAcquire(false, true, /*cursor_capture_enabled=*/false), OdAcquireKind::Ignorable);
}

TEST(ClassifyOdAcquire, NeitherPresentNorMouseUpdateIsIgnorable) {
    EXPECT_EQ(ClassifyOdAcquire(false, false, true), OdAcquireKind::Ignorable);
    EXPECT_EQ(ClassifyOdAcquire(false, false, false), OdAcquireKind::Ignorable);
}
