// test_capture_drain_step.cpp — which capture source the frame loop may drain.
//
// Regression origin: a DXGI OD recording whose monitor was unplugged entered the
// reopen hold (od_holding = true). The frame loop guarded the OD drain with
// `if (use_od_capture && !od_holding)` and let its `else` fall through to the WGC
// drain — which called TryGetNextFrame() on a frame pool that only ever exists for
// WGC captures. For a monitor recording that pool is null, so the call read a vtable
// at address 0 and the process died with an access violation.
//
// Draining "the other backend" is never correct. Holding must drain nothing.

#include <recorder_core/dxgi_od_capture_src.h>

#include <gtest/gtest.h>

using recorder_core::CaptureDrainStep;
using recorder_core::NextCaptureDrainStep;

TEST(CaptureDrainStep, OdCaptureDrainsOd) {
    EXPECT_EQ(NextCaptureDrainStep(/*use_od_capture=*/true, /*od_holding=*/false), CaptureDrainStep::DrainOd);
}

TEST(CaptureDrainStep, WgcCaptureDrainsWgc) {
    EXPECT_EQ(NextCaptureDrainStep(/*use_od_capture=*/false, /*od_holding=*/false), CaptureDrainStep::DrainWgc);
}

// The crash: OD capture while holding must NOT drain the (null) WGC frame pool.
TEST(CaptureDrainStep, OdCaptureWhileHoldingDrainsNothing) {
    EXPECT_EQ(NextCaptureDrainStep(/*use_od_capture=*/true, /*od_holding=*/true), CaptureDrainStep::Hold);
}

// od_holding is meaningless for a WGC capture; it must not divert the drain either.
TEST(CaptureDrainStep, WgcCaptureIgnoresHoldFlag) {
    EXPECT_EQ(NextCaptureDrainStep(/*use_od_capture=*/false, /*od_holding=*/true), CaptureDrainStep::DrainWgc);
}

// The property that actually keeps the process alive: the WGC pool is only ever
// touched by a WGC capture.
TEST(CaptureDrainStep, WgcDrainImpliesWgcCapture) {
    for (const bool use_od : {false, true}) {
        for (const bool holding : {false, true}) {
            if (NextCaptureDrainStep(use_od, holding) == CaptureDrainStep::DrainWgc) {
                EXPECT_FALSE(use_od) << "a monitor capture has no frame pool to drain";
            }
        }
    }
}
