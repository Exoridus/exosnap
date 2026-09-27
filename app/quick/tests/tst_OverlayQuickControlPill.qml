import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The pill is the one capture-excluded overlay that is not click-through, so
// its placement is a correctness property rather than a layout preference.
//
// The default placement and the drag clamp used to be computed against the
// monitor's WORK area (excluding the taskbar). That enforced a safety margin
// the product no longer wants: the dock may be moved all the way to the real
// screen edge and may overlap the ordinary taskbar there. The movement
// boundary is now the full monitor rectangle (monitorGeometry); a separate
// sourceGeometry/sourceIsRegion pair only prefers a placement near an Area
// selection and never bounds movement.
//
// The placement functions never show the window: `visible` is gated on
// CaptureExclusion.granted, which stays false without a real platform window,
// and their assertions are about the geometry bindings rather than about
// anything on screen. The drag functions further down are the exception, and
// say why.
TestCase {
    id: testCase

    name: "OverlayQuickControlPill"
    when: windowShown
    width: 400
    height: 200
    visible: true

    Component {
        id: pillComponent

        OverlayQuickControlPill {
            overlayActive: true
        }
    }

    readonly property rect monitor: Qt.rect(100, 50, 1600, 900)

    function test_default_position_is_bottom_centred_on_the_monitor() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            monitorGeometry: testCase.monitor
        });
        verify(pill);

        compare(pill.x, testCase.monitor.x + (testCase.monitor.width - pill.width) / 2);
        compare(pill.y, testCase.monitor.y + testCase.monitor.height - pill.height - pill.edgeMargin);
    }

    // The inset is a spacing rung, not a number of the pill's own: a theme that
    // moves its scale has to move the overlay with it, and a second literal
    // would silently stop tracking.
    function test_the_edge_margin_is_a_theme_spacing_token() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            monitorGeometry: testCase.monitor
        });
        verify(pill);

        compare(pill.edgeMargin, ExoTheme.spacingSm);
        verify(pill.edgeMargin > 0);
        // The gap is real, not merely declared: the pill's bottom edge stands
        // exactly that far off the monitor's.
        compare(testCase.monitor.y + testCase.monitor.height - (pill.y + pill.height), pill.edgeMargin);
    }

    // No monitor bound (an unresolved or non-monitor capture target) falls back
    // to the same full-screen rectangle the other overlays use.
    function test_an_empty_monitor_geometry_falls_back_to_the_screen() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            monitorGeometry: Qt.rect(0, 0, 0, 0)
        });
        verify(pill);

        compare(pill.effectiveMonitor, Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height));
    }

    // ── Region-mode anchor preference ───────────────────────────────────────
    //
    // sourceGeometry only ever moves the DEFAULT placement; it never bounds
    // movement — that is what effectiveMonitor is for, and these three tests
    // pin the placement order the design spec asks for: below, then above,
    // then the plain monitor default.

    readonly property rect regionWithRoomBelow: Qt.rect(300, 100, 400, 300)
    readonly property rect regionNearTheBottom: Qt.rect(300, 700, 400, 190)
    readonly property rect regionSpanningTheMonitor: Qt.rect(300, 60, 400, 880)

    function test_region_mode_prefers_a_placement_below_the_region() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            monitorGeometry: testCase.monitor,
            sourceGeometry: testCase.regionWithRoomBelow,
            sourceIsRegion: true
        });
        verify(pill);

        const region = testCase.regionWithRoomBelow;
        compare(pill.x, region.x + (region.width - pill.width) / 2);
        compare(pill.y, region.y + region.height + pill.regionGap);
    }

    function test_region_mode_falls_back_to_above_when_there_is_no_room_below() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            monitorGeometry: testCase.monitor,
            sourceGeometry: testCase.regionNearTheBottom,
            sourceIsRegion: true
        });
        verify(pill);

        const region = testCase.regionNearTheBottom;
        compare(pill.y, region.y - pill.regionGap - pill.height);
        verify(pill.y >= testCase.monitor.y, "the above-region placement must still land on the monitor");
    }

    function test_region_mode_falls_back_to_the_monitor_default_when_neither_fits() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            monitorGeometry: testCase.monitor,
            sourceGeometry: testCase.regionSpanningTheMonitor,
            sourceIsRegion: true
        });
        verify(pill);

        compare(pill.y, testCase.monitor.y + testCase.monitor.height - pill.height - pill.edgeMargin);
    }

    // A Window target is also usually smaller than its monitor, but it is not a
    // region: it must get the plain monitor default, not the below/above
    // preference.
    function test_a_non_region_source_rect_does_not_change_the_default_placement() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            monitorGeometry: testCase.monitor,
            sourceGeometry: testCase.regionWithRoomBelow,
            sourceIsRegion: false
        });
        verify(pill);

        compare(pill.x, testCase.monitor.x + (testCase.monitor.width - pill.width) / 2);
        compare(pill.y, testCase.monitor.y + testCase.monitor.height - pill.height - pill.edgeMargin);
    }

    // ── Dragging ─────────────────────────────────────────────────────────────
    //
    // The grip is the only draggable overlay in the product, and its arithmetic
    // is the kind that looks right and is not: the window moves out from under
    // the pointer, so what a move event reports depends on what the previous one
    // did.
    //
    // These functions are the exception to the "never shown" note above.
    // A window that was never shown leaves its content item at 0 x 0, so the
    // grip has no hit area and no mouse event can reach it; `visible` is
    // therefore written directly here, which replaces the CaptureExclusion
    // binding for this one instance. Nothing reaches a screen -- the suite runs
    // on the offscreen platform.
    function shownPill() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            monitorGeometry: testCase.monitor
        });
        verify(pill);
        pill.visible = true;
        tryVerify(() => pill.contentItem.width > 0);
        return pill;
    }

    function gripOf(pill) {
        let grip = pill.contentItem.children[0].children[0];
        verify(grip);
        verify(grip.width > 0 && grip.height > 0);
        return grip;
    }

    // Windows sends a further move whenever a window travels under a pointer
    // that has not itself moved, so one gesture arrives as a burst of events
    // reporting the same desktop position. `steps` replays that burst: the
    // pointer is pinned to one place on the virtual desktop and the local
    // coordinate is recomputed from wherever the window has got to.
    function dragPointerTo(pill, grip, desktopX, desktopY, steps) {
        for (let i = 0; i < steps; ++i)
            mouseMove(grip, desktopX - pill.x - grip.x, desktopY - pill.y - grip.y, 0, Qt.LeftButton);
    }

    // The reported defect: the smallest drag on the grip sent the pill to a
    // corner of the monitor. The pill must instead sit exactly where the
    // pointer carried it, however many events the gesture is delivered as.
    function test_a_small_drag_moves_the_pill_by_the_pointer_delta_and_no_further() {
        let pill = shownPill();
        let grip = gripOf(pill);

        const startX = pill.x;
        const startY = pill.y;
        const pressLocal = Qt.point(14, 30);
        // Up rather than down: the default placement already sits on the
        // bottom clamp, so a downward nudge is refused for a reason that has
        // nothing to do with what this function is about.
        const pointerX = startX + grip.x + pressLocal.x + 8;
        const pointerY = startY + grip.y + pressLocal.y - 6;

        mousePress(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);
        dragPointerTo(pill, grip, pointerX, pointerY, 120);
        mouseRelease(grip, pointerX - pill.x - grip.x, pointerY - pill.y - grip.y, Qt.LeftButton);

        compare(pill.x, startX + 8, "an 8 px drag must move the pill 8 px, not " + (pill.x - startX));
        compare(pill.y, startY - 6, "a 6 px drag must move the pill 6 px, not " + (startY - pill.y));
        // Named separately from the two comparisons above so a regression says
        // which failure it is: the corner is where an accumulating drag ends up.
        verify(pill.x < testCase.monitor.x + testCase.monitor.width - pill.width,
               "the pill ran into the right edge of the monitor");
        verify(pill.y < testCase.monitor.y + testCase.monitor.height - pill.height,
               "the pill ran into the bottom edge of the monitor");
        compare(pill.userPositioned, true);
    }

    // A pointer that keeps moving, which is the ordinary case: each waypoint is
    // an absolute desktop position, and the pill has to end up under the last
    // one rather than at the sum of the distances between them.
    function test_a_drag_across_several_waypoints_ends_under_the_pointer() {
        let pill = shownPill();
        let grip = gripOf(pill);

        const startX = pill.x;
        const startY = pill.y;
        const pressLocal = Qt.point(14, 30);
        const originX = startX + grip.x + pressLocal.x;
        const originY = startY + grip.y + pressLocal.y;

        mousePress(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);
        for (let step = 1; step <= 10; ++step)
            dragPointerTo(pill, grip, originX - 5 * step, originY - 3 * step, 3);
        mouseRelease(grip, originX - 50 - pill.x - grip.x, originY - 30 - pill.y - grip.y, Qt.LeftButton);

        compare(pill.x, startX - 50);
        compare(pill.y, startY - 30);
    }

    // No enforced taskbar safety margin: a drag into the corner stops flush
    // against the real monitor edge, not at a themed inset.
    function test_a_drag_into_the_corner_stops_flush_with_the_monitor_edge() {
        let pill = shownPill();
        let grip = gripOf(pill);

        const pressLocal = Qt.point(14, 30);
        const area = testCase.monitor;

        mousePress(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);
        dragPointerTo(pill, grip, area.x + area.width + 400, area.y + area.height + 400, 6);
        mouseRelease(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);

        compare(pill.x, area.x + area.width - pill.width);
        compare(pill.y, area.y + area.height - pill.height);
    }

    function test_a_drag_into_the_opposite_corner_stops_flush_with_the_monitor_edge() {
        let pill = shownPill();
        let grip = gripOf(pill);

        const pressLocal = Qt.point(14, 30);
        const area = testCase.monitor;

        mousePress(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);
        dragPointerTo(pill, grip, area.x - 400, area.y - 400, 6);
        mouseRelease(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);

        compare(pill.x, area.x);
        compare(pill.y, area.y);
    }

    // A press that never really moves is a click on the grip (collapse/expand),
    // not a drag -- userPositioned must stay false so the default placement
    // keeps tracking the recording target.
    function test_a_bare_click_on_the_grip_does_not_mark_the_pill_user_positioned() {
        let pill = shownPill();
        let grip = gripOf(pill);
        const wasExpanded = pill.expanded;

        mousePress(grip, 14, 30, Qt.LeftButton);
        mouseRelease(grip, 14, 30, Qt.LeftButton);

        compare(pill.userPositioned, false);
        compare(pill.expanded, !wasExpanded);
    }

    // A user-placed pill does not re-run the default placement when the
    // recording target's monitor changes -- the drag already overwrote the x/y
    // bindings -- so its offset has to be re-clamped by hand or it could sit
    // off-screen (or simply off the new target's monitor) after the target
    // moves to a smaller or differently-positioned display.
    function test_a_user_placed_pill_is_re_clamped_after_a_monitor_change() {
        let pill = shownPill();
        let grip = gripOf(pill);

        const pressLocal = Qt.point(14, 30);
        mousePress(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);
        dragPointerTo(pill, grip, testCase.monitor.x + testCase.monitor.width + 400,
                      testCase.monitor.y + testCase.monitor.height + 400, 6);
        mouseRelease(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);
        verify(pill.userPositioned);

        // The recording moved to a smaller monitor at a different origin.
        const smaller = Qt.rect(0, 0, 800, 500);
        pill.monitorGeometry = smaller;

        compare(pill.x, smaller.x + smaller.width - pill.width);
        compare(pill.y, smaller.y + smaller.height - pill.height);
    }

    // ── Close button ─────────────────────────────────────────────────────────
    //
    // Persisting "closed" is Main.qml's job (it turns showQuickControls off);
    // this only pins the button's own contract -- the last button in the row
    // emits closeRequested(), once, per click.
    function test_the_close_button_emits_close_requested() {
        let pill = shownPill();
        // contentItem.children[0] is the Rectangle; .children[1] inside it is
        // the button Row, the same nesting gripOf() reaches .children[0] (the
        // grip Item) through.
        let buttons = pill.contentItem.children[0].children[1];
        verify(buttons);
        let closeButton = buttons.children[buttons.children.length - 1];
        verify(closeButton);

        let spy = createTemporaryObject(signalSpyComponent, testCase, {target: pill, signalName: "closeRequested"});
        verify(spy);

        mouseClick(closeButton, closeButton.width / 2, closeButton.height / 2, Qt.LeftButton);

        compare(spy.count, 1);
    }

    Component {
        id: signalSpyComponent

        SignalSpy {}
    }
}
