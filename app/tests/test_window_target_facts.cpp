// Pure resolvers behind the exclusive-fullscreen pre-flight card (S1):
//   ClassifyWindowShape       — geometry/style -> Normal | FullscreenShaped
//   CombineFullscreenEvidence — shape + hub evidence + signal -> severity ladder
//
// Borderless and FSE are the same *shape*, so shape alone never yields a card;
// the ladder is pinned so a legitimately static borderless window cannot be
// misclassified as ProvenBlack.

#include "diagnostics/WindowTargetFacts.h"

#include <gtest/gtest.h>

using namespace exosnap::diagnostics;
using recorder_core::HubFrameKind;

namespace {

// A fullscreen-shaped window on a 1920x1080 primary monitor with no caption/frame.
WindowTargetFacts fullscreenShaped() {
    WindowTargetFacts f;
    f.valid = true;
    f.visible = true;
    f.minimized = false;
    f.cloaked = false;
    f.window_rect = RECT{0, 0, 1920, 1080};
    f.monitor_rect = RECT{0, 0, 1920, 1080};
    f.style = WS_POPUP | WS_VISIBLE; // borderless/FSE popup: no caption, no frame
    return f;
}

WindowHubEvidence noHubEvidence() {
    return WindowHubEvidence{HubFrameKind::None, 0.0, 0.0, false};
}

TEST(WindowShape, BorderlessCoveringMonitorIsFullscreenShaped) {
    EXPECT_EQ(ClassifyWindowShape(fullscreenShaped()), WindowShape::FullscreenShaped);
}

TEST(WindowShape, OnePixelOverhangStillFullscreenShaped) {
    // Some borderless windows sit 1px outside the monitor on every edge.
    WindowTargetFacts f = fullscreenShaped();
    f.window_rect = RECT{-1, -1, 1921, 1081};
    EXPECT_EQ(ClassifyWindowShape(f), WindowShape::FullscreenShaped);
}

TEST(WindowShape, SecondaryMonitorRectIsFullscreenShaped) {
    WindowTargetFacts f = fullscreenShaped();
    f.window_rect = RECT{1920, 0, 3840, 1080};
    f.monitor_rect = RECT{1920, 0, 3840, 1080};
    EXPECT_EQ(ClassifyWindowShape(f), WindowShape::FullscreenShaped);
}

TEST(WindowShape, CaptionedWindowIsNormalEvenWhenMaximized) {
    WindowTargetFacts f = fullscreenShaped();
    f.style = WS_OVERLAPPEDWINDOW | WS_VISIBLE; // includes WS_CAPTION + WS_THICKFRAME
    EXPECT_EQ(ClassifyWindowShape(f), WindowShape::Normal);
}

TEST(WindowShape, ResizeFrameOnlyIsNormal) {
    WindowTargetFacts f = fullscreenShaped();
    f.style = WS_POPUP | WS_THICKFRAME | WS_VISIBLE;
    EXPECT_EQ(ClassifyWindowShape(f), WindowShape::Normal);
}

TEST(WindowShape, WindowNotCoveringMonitorIsNormal) {
    WindowTargetFacts f = fullscreenShaped();
    f.window_rect = RECT{100, 100, 900, 700}; // leaves monitor edges exposed
    EXPECT_EQ(ClassifyWindowShape(f), WindowShape::Normal);
}

TEST(WindowShape, MinimizedIsNormal) {
    WindowTargetFacts f = fullscreenShaped();
    f.minimized = true;
    EXPECT_EQ(ClassifyWindowShape(f), WindowShape::Normal);
}

TEST(WindowShape, CloakedIsNormal) {
    WindowTargetFacts f = fullscreenShaped();
    f.cloaked = true;
    EXPECT_EQ(ClassifyWindowShape(f), WindowShape::Normal);
}

TEST(WindowShape, InvalidIsNormal) {
    WindowTargetFacts f{}; // valid == false
    EXPECT_EQ(ClassifyWindowShape(f), WindowShape::Normal);
}

// ---- CombineFullscreenEvidence severity ladder ----

TEST(FullscreenEvidence, ShapeAloneIsNothing) {
    // Borderless that works: fullscreen-shaped, no signal, no bad hub evidence.
    EXPECT_EQ(CombineFullscreenEvidence(WindowShape::FullscreenShaped, noHubEvidence(), false),
              ExclusiveEvidence::None);
}

TEST(FullscreenEvidence, NormalShapeNeverReports) {
    WindowHubEvidence hub{HubFrameKind::None, 10.0, 0.0, false};
    EXPECT_EQ(CombineFullscreenEvidence(WindowShape::Normal, hub, true), ExclusiveEvidence::None);
}

TEST(FullscreenEvidence, FullscreenSignalGivesSuspected) {
    // QUNS or PresentMon FSE, but no measured black proof yet.
    EXPECT_EQ(CombineFullscreenEvidence(WindowShape::FullscreenShaped, noHubEvidence(), true),
              ExclusiveEvidence::Suspected);
}

TEST(FullscreenEvidence, NoneUnderTwoSecondsIsNotProven) {
    // WGC delivers an initial frame; None must persist >= 2 s to count.
    WindowHubEvidence hub{HubFrameKind::None, 1.0, 0.0, false};
    EXPECT_EQ(CombineFullscreenEvidence(WindowShape::FullscreenShaped, hub, false), ExclusiveEvidence::None);
}

TEST(FullscreenEvidence, NoneAtTwoSecondsIsProvenBlack) {
    WindowHubEvidence hub{HubFrameKind::None, 2.0, 0.0, false};
    EXPECT_EQ(CombineFullscreenEvidence(WindowShape::FullscreenShaped, hub, false), ExclusiveEvidence::ProvenBlack);
}

TEST(FullscreenEvidence, HeldFrozenSinceTransitionIsProvenBlack) {
    // Window previewed windowed, then switched to FSE: the hub holds the last
    // good frame and no fresh frame has arrived since the shape transition.
    WindowHubEvidence hub{HubFrameKind::Held, 30.0, 3.0, false};
    EXPECT_EQ(CombineFullscreenEvidence(WindowShape::FullscreenShaped, hub, false), ExclusiveEvidence::ProvenBlack);
}

TEST(FullscreenEvidence, HeldWithFreshFrameAfterTransitionIsNotProven) {
    // Legitimately static borderless (paused fullscreen video): it produced a
    // frame AFTER becoming fullscreen-shaped, so it is not black.
    WindowHubEvidence hub{HubFrameKind::Held, 30.0, 3.0, /*fresh_since_shape=*/true};
    EXPECT_EQ(CombineFullscreenEvidence(WindowShape::FullscreenShaped, hub, false), ExclusiveEvidence::None);
}

TEST(FullscreenEvidence, HeldFrozenUnderTwoSecondsIsNotProven) {
    WindowHubEvidence hub{HubFrameKind::Held, 30.0, 1.0, false};
    EXPECT_EQ(CombineFullscreenEvidence(WindowShape::FullscreenShaped, hub, false), ExclusiveEvidence::None);
}

TEST(FullscreenEvidence, ProvenBlackWinsOverMissingSignal) {
    // Even without QUNS/PresentMon, measured black proof is a blocker.
    WindowHubEvidence hub{HubFrameKind::None, 5.0, 0.0, false};
    EXPECT_EQ(CombineFullscreenEvidence(WindowShape::FullscreenShaped, hub, false), ExclusiveEvidence::ProvenBlack);
}

TEST(GatherFacts, NullHandleIsInvalid) {
    const WindowTargetFacts f = GatherWindowTargetFacts(nullptr);
    EXPECT_FALSE(f.valid);
}

} // namespace
