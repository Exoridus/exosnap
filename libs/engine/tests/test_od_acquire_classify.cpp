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

// ---------------------------------------------------------------------------
// ShouldRecoverIdleOdAcquire -- a duplication left behind by a topology change
// keeps returning WAIT_TIMEOUT, which is what an untouched desktop returns too.
// ---------------------------------------------------------------------------

// REGRESSION: the recording sat on a duplication that could not deliver, and
// (before the first frame) died on the 5 s guard with zero captured frames.
TEST(ShouldRecoverIdleOd, AChangedTopologyStartsARecovery) {
    EXPECT_TRUE(ShouldRecoverIdleOdAcquire(/*already_recovering=*/false, /*topology_changed=*/true));
}

// The load-bearing half: an ordinary static desktop delivers nothing for as long
// as nobody touches it, and must never have its duplication rebuilt for it.
TEST(ShouldRecoverIdleOd, AStaticDesktopIsNeverRecovered) {
    EXPECT_FALSE(ShouldRecoverIdleOdAcquire(/*already_recovering=*/false, /*topology_changed=*/false));
}

// One attempt per change: while a recovery is already running, further silent
// polls must not restart it (the retry throttle owns the cadence from there).
TEST(ShouldRecoverIdleOd, ARunningRecoveryIsNotRestarted) {
    EXPECT_FALSE(ShouldRecoverIdleOdAcquire(/*already_recovering=*/true, /*topology_changed=*/true));
}
