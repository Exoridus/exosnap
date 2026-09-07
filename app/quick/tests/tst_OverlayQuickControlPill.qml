import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The pill is the one capture-excluded overlay that is not click-through, so a
// part of it sitting under the taskbar is an unclickable control, not merely a
// cosmetic overlap. Before this test, the default position and the drag clamp
// were computed against the full monitor rectangle
// (OverlayAdapter::recordedMonitorGeometry), which includes the taskbar; a
// monitor with a taller taskbar than the pill's own edge margin left the bottom
// of the pill behind it.
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

    // A monitor whose taskbar takes 96 px off the bottom -- taller than the
    // pill's 32 px default margin, so a bug that measures from the full
    // rectangle instead of the work area is caught rather than hidden by a
    // taskbar that happens to be thinner than the margin.
    readonly property rect workArea: Qt.rect(100, 50, 1600, 804)
    readonly property rect fullMonitor: Qt.rect(100, 50, 1600, 900)

    function test_default_position_keeps_the_pill_inside_the_work_area() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            workAreaGeometry: testCase.workArea
        });
        verify(pill);

        const bottom = pill.y + pill.height;
        verify(bottom <= testCase.workArea.y + testCase.workArea.height,
               "pill bottom " + bottom + " must stay above the work area's bottom edge "
               + (testCase.workArea.y + testCase.workArea.height));
        // The same edge measured against the full monitor rectangle would sit
        // 96 px lower, inside the taskbar -- this is the regression itself.
        verify(bottom < testCase.fullMonitor.y + testCase.fullMonitor.height - pill.screenMargin);
    }

    function test_default_position_is_bottom_centred_on_the_work_area() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            workAreaGeometry: testCase.workArea
        });
        verify(pill);

        compare(pill.x, testCase.workArea.x + (testCase.workArea.width - pill.width) / 2);
        compare(pill.y, testCase.workArea.y + testCase.workArea.height - pill.height - pill.screenMargin);
    }

    // The inset is a spacing rung, not a number of the pill's own: a theme that
    // moves its scale has to move the overlay with it, and a second literal 32
    // would silently stop tracking.
    function test_the_screen_margin_is_a_theme_spacing_token() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            workAreaGeometry: testCase.workArea
        });
        verify(pill);

        compare(pill.screenMargin, ExoTheme.spacing2Xl);
        verify(pill.screenMargin > 0);
        // The gap is real, not merely declared: the pill's bottom edge stands
        // exactly that far off the work area's.
        compare(testCase.workArea.y + testCase.workArea.height - (pill.y + pill.height), pill.screenMargin);
    }

    // No monitor bound (an unresolved or non-monitor capture target) falls back
    // to the same full-screen rectangle the other overlays use -- unchanged by
    // this fix, and worth pinning down because it is the one path with no
    // C++-supplied rectangle to assert against.
    function test_an_empty_work_area_falls_back_to_the_screen() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            workAreaGeometry: Qt.rect(0, 0, 0, 0)
        });
        verify(pill);

        compare(pill.effectiveWorkArea, Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height));
    }

    // ── Dragging ─────────────────────────────────────────────────────────────
    //
    // The grip is the only draggable overlay in the product, and its arithmetic
    // is the kind that looks right and is not: the window moves out from under
    // the pointer, so what a move event reports depends on what the previous one
    // did.
    //
    // These four functions are the exception to the "never shown" note above.
    // A window that was never shown leaves its content item at 0 x 0, so the
    // grip has no hit area and no mouse event can reach it; `visible` is
    // therefore written directly here, which replaces the CaptureExclusion
    // binding for this one instance. Nothing reaches a screen -- the suite runs
    // on the offscreen platform.
    function shownPill() {
        let pill = createTemporaryObject(pillComponent, testCase, {
            workAreaGeometry: testCase.workArea
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
    // corner of the work area. The pill must instead sit exactly where the
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
        verify(pill.x < testCase.workArea.x + testCase.workArea.width - pill.width - pill.screenMargin,
               "the pill ran into the right edge of the work area");
        verify(pill.y < testCase.workArea.y + testCase.workArea.height - pill.height - pill.screenMargin,
               "the pill ran into the bottom edge of the work area");
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

    // The drag clamp uses the same inset as the default placement, so a pill
    // dragged hard into the corner still stands off the work-area edge rather
    // than touching it.
    function test_a_drag_into_the_corner_stops_at_the_themed_margin() {
        let pill = shownPill();
        let grip = gripOf(pill);

        const pressLocal = Qt.point(14, 30);
        const area = testCase.workArea;

        mousePress(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);
        dragPointerTo(pill, grip, area.x + area.width + 400, area.y + area.height + 400, 6);
        mouseRelease(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);

        compare(pill.x, area.x + area.width - pill.width - pill.screenMargin);
        compare(pill.y, area.y + area.height - pill.height - pill.screenMargin);
    }

    function test_a_drag_into_the_opposite_corner_stops_at_the_themed_margin() {
        let pill = shownPill();
        let grip = gripOf(pill);

        const pressLocal = Qt.point(14, 30);
        const area = testCase.workArea;

        mousePress(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);
        dragPointerTo(pill, grip, area.x - 400, area.y - 400, 6);
        mouseRelease(grip, pressLocal.x, pressLocal.y, Qt.LeftButton);

        compare(pill.x, area.x + pill.screenMargin);
        compare(pill.y, area.y + pill.screenMargin);
    }
}
