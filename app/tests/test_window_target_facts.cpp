// Pure resolvers behind the exclusive-fullscreen pre-flight card (S1):
//   ClassifyWindowShape       — geometry/style -> Normal | FullscreenShaped
//   CombineFullscreenEvidence — shape + hub evidence + signal -> severity ladder
//
// Borderless and FSE are the same *shape*, so shape alone never yields a card;
// the ladder is pinned so a legitimately static borderless window cannot be
// misclassified as ProvenBlack.

#include "diagnostics/WindowEvidenceSnapshot.h"
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

// ── QCR-110: the one resolver both consumers go through ─────────────────────────
//
// ResolveExclusiveEvidence is what stops the Diagnostics card and the recording
// admission gate from drifting: neither re-assembles the ladder from its parts.

TEST(ResolveEvidence, MatchesTheHandAssembledLadder) {
    const WindowTargetFacts f = fullscreenShaped();
    const WindowHubEvidence hub{HubFrameKind::None, 5.0, 0.0, false};
    EXPECT_EQ(ResolveExclusiveEvidence(f, hub, false),
              CombineFullscreenEvidence(ClassifyWindowShape(f), hub, f.quns_d3d_fullscreen));
}

TEST(ResolveEvidence, QunsSignalAloneIsSuspectedNotProven) {
    WindowTargetFacts f = fullscreenShaped();
    f.quns_d3d_fullscreen = true;
    // Producing frames: shaped like fullscreen and flagged by the shell, but not
    // black. A Notice, never a start blocker.
    const WindowHubEvidence hub{HubFrameKind::Live, 5.0, 0.0, true};
    EXPECT_EQ(ResolveExclusiveEvidence(f, hub, false), ExclusiveEvidence::Suspected);
}

TEST(ResolveEvidence, PresentSignalIsOredWithQuns) {
    WindowTargetFacts f = fullscreenShaped();
    f.quns_d3d_fullscreen = false;
    const WindowHubEvidence hub{HubFrameKind::Live, 5.0, 0.0, true};
    EXPECT_EQ(ResolveExclusiveEvidence(f, hub, false), ExclusiveEvidence::None);
    EXPECT_EQ(ResolveExclusiveEvidence(f, hub, true), ExclusiveEvidence::Suspected);
}

TEST(ResolveEvidence, NormalWindowIsNeverAVerdict) {
    WindowTargetFacts f = fullscreenShaped();
    f.style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    f.quns_d3d_fullscreen = true;
    const WindowHubEvidence hub{HubFrameKind::None, 30.0, 0.0, false};
    EXPECT_EQ(ResolveExclusiveEvidence(f, hub, true), ExclusiveEvidence::None);
}

// ── QCR-110: the probe snapshot contract ────────────────────────────────────────
//
// The snapshot names the window it describes, so a consumer that has retargeted
// cannot judge the new target by the old one's measurements. Everything that
// cannot speak for the asked-about window resolves to None — never a guess.

WindowEvidenceSnapshot provenBlackSnapshot(uintptr_t hwnd) {
    WindowEvidenceSnapshot snapshot;
    snapshot.active = true;
    snapshot.hwnd = hwnd;
    snapshot.facts = fullscreenShaped();
    snapshot.evidence = WindowHubEvidence{HubFrameKind::None, 5.0, 0.0, false};
    return snapshot;
}

TEST(SnapshotEvidence, NoProducerIsNoneNotProvenBlack) {
    // The default-constructed snapshot is what every consumer sees before the
    // probe exists at all. It must never be mistaken for measured proof.
    EXPECT_EQ(ResolveSnapshotEvidence(WindowEvidenceSnapshot{}, 0x1234, false), ExclusiveEvidence::None);
}

TEST(SnapshotEvidence, InactiveSnapshotIsNone) {
    WindowEvidenceSnapshot snapshot = provenBlackSnapshot(0x1234);
    snapshot.active = false;
    EXPECT_EQ(ResolveSnapshotEvidence(snapshot, 0x1234, false), ExclusiveEvidence::None);
}

TEST(SnapshotEvidence, MatchingTargetResolvesProvenBlack) {
    EXPECT_EQ(ResolveSnapshotEvidence(provenBlackSnapshot(0x1234), 0x1234, false), ExclusiveEvidence::ProvenBlack);
}

TEST(SnapshotEvidence, RetargetInvalidatesTheOldWindowsEvidence) {
    // The proof belongs to 0x1234. Asking about the window the user just picked
    // must not inherit it, however recent the snapshot is.
    EXPECT_EQ(ResolveSnapshotEvidence(provenBlackSnapshot(0x1234), 0x5678, false), ExclusiveEvidence::None);
}

TEST(SnapshotEvidence, ZeroTargetIsNone) {
    // A monitor target, or no selection at all.
    EXPECT_EQ(ResolveSnapshotEvidence(provenBlackSnapshot(0x1234), 0, false), ExclusiveEvidence::None);
}

TEST(SnapshotEvidence, UnknownWindowIsNotBlocked) {
    // Subscribed, on the right window, but nothing measured yet: an ordinary
    // window the probe has only just been pointed at must start recording.
    WindowEvidenceSnapshot snapshot;
    snapshot.active = true;
    snapshot.hwnd = 0x1234;
    snapshot.facts = fullscreenShaped();
    snapshot.evidence = WindowHubEvidence{HubFrameKind::None, 0.2, 0.0, false};
    EXPECT_EQ(ResolveSnapshotEvidence(snapshot, 0x1234, false), ExclusiveEvidence::None);
}

} // namespace
