import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "EditTimelineKeyboard"
    when: windowShown
    width: 640
    height: 240
    visible: true
    Component {
        id: textInputComponent
        TextInput { width: 200; height: 30; text: "Editable name" }
    }
    Component {
        id: timelineComponent
        EditTimeline {
            session: testSession
            player: testTransportPlayer
            width: 600
            height: 200
        }
    }
    function init() {
        testTransportPlayer.setPlaying(false);
        testSession.cancelClipDrag();
        testSession.snapping = true;
        testSession.ripple = false;
        testSession.autoCrossfade = true;
        testSession.linkedSelection = true;
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        testSession.requestSeek(0);
    }
    function make() {
        const strip = createTemporaryObject(timelineComponent, testCase);
        verify(strip);
        strip.forceActiveFocus();
        verify(strip.activeFocus);
        return strip;
    }
    function test_space_toggles_only_on_release() {
        make();
        const before = testEditHarness.transportCommand(testTransportPlayer);
        keyPress(Qt.Key_Space);
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
        keyRelease(Qt.Key_Space);
        compare(testEditHarness.transportCommand(testTransportPlayer), before + 1);
    }
    function test_space_pan_never_toggles_or_seeks() {
        const strip = make();
        strip.zoomTo(100);
        strip.panTimeline(500);
        const viewport = findChild(strip, "editViewport");
        const start = viewport.startUs;
        const position = testSession.positionUs;
        const before = testEditHarness.transportCommand(testTransportPlayer);
        keyPress(Qt.Key_Space);
        mousePress(strip, 350, 80, Qt.LeftButton);
        mouseMove(strip, 250, 80, 20);
        mouseRelease(strip, 250, 80, Qt.LeftButton);
        keyRelease(Qt.Key_Space);
        verify(viewport.startUs > start);
        compare(testSession.positionUs, position);
        compare(testSession.dragActive, false);
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
    }
    function test_overview_pan_capture_reserves_space_without_lane_pan() {
        const strip = make();
        strip.zoomTo(100);
        strip.panTimeline(500);
        const controller = findChild(strip, "editInteraction");
        const viewport = findChild(strip, "editViewport");
        const start = viewport.startUs;
        const before = testEditHarness.transportCommand(testTransportPlayer);
        keyPress(Qt.Key_Space);
        controller.beginPan(Qt.point(200, 80), false);
        verify(controller.pointerCaptured);
        verify(!controller.panActive);
        controller.updatePan(Qt.point(100 - Qt.styleHints.startDragDistance, 80));
        verify(controller.panActive);
        compare(viewport.startUs, start);
        controller.endPan();
        keyRelease(Qt.Key_Space);
        verify(!controller.panActive);
        verify(!controller.pointerCaptured);
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
    }
    function test_space_repeat_and_modifiers() {
        const strip = make();
        const before = testEditHarness.transportCommand(testTransportPlayer);
        keyClick(Qt.Key_Space, Qt.ControlModifier);
        keyClick(Qt.Key_Space, Qt.AltModifier);
        keyClick(Qt.Key_Space, Qt.ShiftModifier);
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
        keyPress(Qt.Key_Space);
        for (let i = 0; i < 10; ++i) testEditHarness.repeatSpace(strip);
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
        keyRelease(Qt.Key_Space);
        compare(testEditHarness.transportCommand(testTransportPlayer), before + 1);
    }
    function test_nested_text_input_keeps_space() {
        const strip = make();
        const input = createTemporaryObject(textInputComponent, strip);
        input.forceActiveFocus();
        input.cursorPosition = input.text.length;
        const before = testEditHarness.transportCommand(testTransportPlayer);
        keyClick(Qt.Key_Space);
        compare(input.text, "Editable name ");
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
    }
    Component { id: localSlider; Slider { width: 150; height: 30 } }
    Component { id: localCombo; ComboBox { width: 150; model: ["One", "Two"] } }
    function test_local_control_keeps_space_data() {
        return [{tag: "slider", component: localSlider}, {tag: "combo", component: localCombo}];
    }
    function test_local_control_keeps_space(data) {
        const strip = make();
        const control = createTemporaryObject(data.component, strip);
        verify(!!control);
        control.forceActiveFocus();
        verify(control.activeFocus);
        const before = testEditHarness.transportCommand(testTransportPlayer);
        keyClick(Qt.Key_Space);
        compare(findChild(strip, "editInteraction").spaceHeld, false);
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
    }
    function test_cancel_space_data() {
        return [{tag: "focus"}, {tag: "hidden"}, {tag: "deactivate"}, {tag: "escape"}, {tag: "menu"}];
    }
    function test_cancel_space(data) {
        const strip = make();
        const before = testEditHarness.transportCommand(testTransportPlayer);
        keyPress(Qt.Key_Space);
        if (data.tag === "focus") {
            const input = createTemporaryObject(textInputComponent, strip);
            input.forceActiveFocus();
        } else if (data.tag === "hidden") strip.visible = false;
        else if (data.tag === "deactivate") testEditHarness.deactivateWindow(strip);
        else if (data.tag === "escape") keyClick(Qt.Key_Escape);
        else keyClick(Qt.Key_Menu);
        keyRelease(Qt.Key_Space);
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
        compare(findChild(strip, "editInteraction").spaceHeld, false);
    }
    function test_pan_release_orders_data() {
        return [{tag: "space-first"}, {tag: "pointer-first"}, {tag: "escape"}, {tag: "click-only"}];
    }
    function test_pan_release_orders(data) {
        const strip = make();
        strip.zoomTo(100);
        strip.panTimeline(500);
        const viewport = findChild(strip, "editViewport");
        const before = testEditHarness.transportCommand(testTransportPlayer);
        const position = testSession.positionUs;
        const selected = testSession.selectedClip;
        keyPress(Qt.Key_Space);
        mousePress(strip, 350, 80, Qt.LeftButton);
        if (data.tag !== "click-only") mouseMove(strip, 300, 80, 20);
        if (data.tag === "escape") keyClick(Qt.Key_Escape);
        if (data.tag === "space-first" || data.tag === "escape") keyRelease(Qt.Key_Space);
        const start = viewport.startUs;
        if (data.tag === "space-first" || data.tag === "escape") {
            mouseMove(strip, 200, 80, 20);
            compare(viewport.startUs, start);
        }
        mouseRelease(strip, 300, 80, Qt.LeftButton);
        if (data.tag !== "space-first" && data.tag !== "escape") keyRelease(Qt.Key_Space);
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
        compare(testSession.positionUs, position);
        compare(testSession.selectedClip, selected);
        compare(testSession.dragActive, false);
        compare(findChild(strip, "editInteraction").pointerCaptured, false);
    }
    function test_space_during_existing_clip_drag_does_not_arm() {
        const strip = make();
        testSession.beginClipDrag(testSession.selectedClip);
        verify(testSession.dragActive);
        const before = testEditHarness.transportCommand(testTransportPlayer);
        keyClick(Qt.Key_Space);
        compare(findChild(strip, "editInteraction").spaceHeld, false);
        compare(testEditHarness.transportCommand(testTransportPlayer), before);
        verify(testSession.dragActive);
        testSession.cancelClipDrag();
    }
    function test_pan_reverses_immediately_at_start() {
        const strip = make();
        strip.zoomTo(100);
        keyPress(Qt.Key_Space);
        mousePress(strip, 300, 80, Qt.LeftButton);
        mouseMove(strip, 400, 80, 20);
        const viewport = findChild(strip, "editViewport");
        compare(viewport.startUs, 0);
        mouseMove(strip, 380, 80, 20);
        verify(viewport.startUs > 0);
        mouseRelease(strip, 380, 80, Qt.LeftButton);
        keyRelease(Qt.Key_Space);
    }
    function test_focus_and_seek() {
        const strip = make();
        compare(strip.activeFocusOnTab, true);
        verify(strip.Accessible.name.length > 0);
        keyClick(Qt.Key_Right);
        compare(testSession.positionUs, 16666);
        keyClick(Qt.Key_Right, Qt.ShiftModifier);
        compare(testSession.positionMs, 1016);
        keyClick(Qt.Key_End);
        compare(testSession.positionMs, testSession.durationMs);
        keyClick(Qt.Key_Home);
        compare(testSession.positionMs, 0);
    }
    function test_text_input_keeps_edit_shortcuts() {
        const strip = make();
        testSession.requestSeek(50000);
        const input = createTemporaryObject(textInputComponent, strip);
        input.forceActiveFocus();
        verify(input.activeFocus);
        keyClick(Qt.Key_B, Qt.ControlModifier);
        keyClick(Qt.Key_Delete);
        compare(testSession.visibleClips(0, 100000).length, 2);
    }
    function test_io_boundaries_preserve_fractional_frame_microseconds() {
        make();
        keyClick(Qt.Key_Right);
        compare(testSession.positionUs, 16666);
        keyClick(Qt.Key_I);
        compare(testSession.timeSelectionInUs, 16666);
        compare(testSession.timeSelectionInMs, 16);
        keyClick(Qt.Key_Right);
        compare(testSession.positionUs, 33333);
        keyClick(Qt.Key_O);
        compare(testSession.timeSelectionOutUs, 33333);
        compare(testSession.timeSelectionOutMs, 33);
        compare(testSession.timeSelectionOutUs - testSession.timeSelectionInUs, 16667);
    }
    function test_split_and_undo_redo() {
        make();
        testSession.requestSeek(50000);
        keyClick(Qt.Key_B, Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000).length, 4);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000).length, 2);
        keyClick(Qt.Key_Y, Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000).length, 4);
    }
    function test_keyboard_trim_move_and_delete() {
        make();
        keyClick(Qt.Key_Comma, Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000)[0].inUs, 16666);
        keyClick(Qt.Key_Period);
        compare(testSession.visibleClips(0, 100000)[0].startUs, 33333);
        keyClick(Qt.Key_Delete);
        compare(testSession.visibleClips(0, 100000).length, 0);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(testSession.visibleClips(0, 100000).length, 2);
    }
    function test_alt_sample_step_does_not_move_linked_video_selection() {
        make();
        const rows = testSession.visibleClipsUs(0, 100000000);
        keyClick(Qt.Key_Right, Qt.AltModifier);
        compare(testSession.positionUs, 0);
        compare(testSession.visibleClipsUs(0, 100000000).map(row => row.startUs), rows.map(row => row.startUs));
    }
    function test_clip_clipboard_keys_paste_complete_linked_group() {
        const strip = createTemporaryObject(timelineComponent, testCase, {session: testClipboardSession});
        verify(!!strip);
        strip.forceActiveFocus();
        const session = strip.session;
        session.selectAllClips();
        compare(session.selectedClipIds.length, 2);
        keyClick(Qt.Key_C, Qt.ControlModifier);
        verify(session.hasClipClipboard);
        keyClick(Qt.Key_End);
        compare(session.positionUs, session.durationUs);
        keyClick(Qt.Key_V, Qt.ControlModifier);
        compare(session.workspaceError, "");
        verify(session.dragActive && session.dragValid);
        keyClick(Qt.Key_Return);
        compare(session.visibleClipsUs(0, 200000000).length, 4);
        compare(session.selectedClipIds.length, 2);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(session.visibleClipsUs(0, 200000000).length, 2);
    }
    function test_clip_paste_is_visible_candidate_until_enter_and_escape_restores_selection() {
        const strip = createTemporaryObject(timelineComponent, testCase, {session: testClipboardSession});
        strip.forceActiveFocus();
        const session = strip.session;
        session.cancelClipDrag();
        session.selectAllClips();
        const before = session.visibleClipsUs(0, 200000000).length;
        const selected = session.selectedClipIds.slice();
        keyClick(Qt.Key_C, Qt.ControlModifier);
        keyClick(Qt.Key_End);
        try {
            keyClick(Qt.Key_V, Qt.ControlModifier);
            compare(session.visibleClipsUs(0, 200000000).length, before);
            verify(session.dragActive && session.dragValid);
            compare(session.dragClips.length, 2);
            waitForRendering(strip);
            const ghost = findChild(strip, "editDragGhost" + session.dragClips[0].id);
            verify(!!ghost && ghost.width > 0);
            verify(strip.view.endUs >= session.dragClips[0].endUs);
            keyClick(Qt.Key_Escape);
            compare(session.selectedClipIds, selected);
            compare(session.visibleClipsUs(0, 200000000).length, before);
            keyClick(Qt.Key_V, Qt.ControlModifier);
            verify(session.dragActive);
            keyClick(Qt.Key_Return);
            compare(session.visibleClipsUs(0, 200000000).length, before + 2);
        } finally {
            session.cancelClipDrag();
            if (session.visibleClipsUs(0, 200000000).length > before)
                session.undo();
        }
    }
    function test_boundary_navigation_and_escape_clear_one_selection_owner() {
        make();
        testSession.requestSeekUs(50000000);
        testSession.splitSelected();
        testSession.requestSeekUs(25000000);
        keyClick(Qt.Key_Down);
        compare(testSession.positionUs, 50000000);
        keyClick(Qt.Key_I);
        keyClick(Qt.Key_Right, Qt.ShiftModifier);
        keyClick(Qt.Key_O);
        compare(testSession.selectionMode, EditSessionAdapter.RangeSelection);
        keyClick(Qt.Key_Escape);
        compare(testSession.selectionMode, EditSessionAdapter.NoSelection);
    }
    function test_keyboard_ripple_closes_gap() {
        make();
        testSession.requestSeek(50000);
        keyClick(Qt.Key_B, Qt.ControlModifier);
        keyClick(Qt.Key_Delete, Qt.ShiftModifier);
        compare(testSession.durationMs, 50000);
        compare(testSession.visibleClips(0, 100000)[0].startMs, 0);
    }
    function test_viewport_bounds_objects_for_long_timeline() {
        make();
        testSession.appendAsset(testSession.media[0].id);
        compare(testSession.visibleClips(0, 99999).length, 2);
        compare(testSession.visibleClips(100001, 200000).length, 2);
    }
}
