import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "EditOverview"
    when: windowShown
    width: 1100
    height: 240
    visible: true
    Item {
        id: canvas
        width: 1050; height: 200
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Space)
                event.accepted = controller.armSpace(event.modifiers, event.isAutoRepeat);
        }
    }
    EditViewportAdapter { id: viewport; timelineEndUs: 100000000; usableWidth: 1000 }
    EditInteractionController { id: controller; scopeItem: canvas }
    Component {
        id: overviewComponent
        EditOverview {
            width: 1020
            height: 12
            session: testSession
            player: testTransportPlayer
            view: viewport
            interaction: controller
            canvasFocusItem: canvas
        }
    }
    function make() {
        const item = createTemporaryObject(overviewComponent, testCase);
        verify(!!item);
        return item;
    }
    function init() {
        controller.cancel();
        viewport.endOverviewOperation();
        viewport.setVisibleInterval(20000000, 40000000);
        canvas.forceActiveFocus();
        testTransportPlayer.setPlaying(false);
    }
    function test_drag_only_moves_view_and_double_click_fits() {
        const item = make();
        const position = testSession.positionUs;
        const command = testEditHarness.transportCommand(testTransportPlayer);
        mousePress(item, 300, 6);
        mouseMove(item, 400, 6);
        mouseRelease(item, 400, 6);
        compare(viewport.startUs, 30000000);
        compare(viewport.endUs, 50000000);
        compare(testSession.positionUs, position);
        compare(testEditHarness.transportCommand(testTransportPlayer), command);
        mouseDoubleClickSequence(item, 400, 6);
        compare(viewport.startUs, 0);
        compare(viewport.endUs, 102000000);
    }
    function test_tiny_window_keeps_exact_rendered_width() {
        const item = make();
        viewport.setVisibleInterval(20000000, 20001000);
        const window = findChild(item, "editOverviewWindow");
        verify(!!window);
        verify(window.width < 1);
        verify(window.width > 0);
    }
    function test_space_press_consumes_toggle_and_early_space_up_ends_pan() {
        const item = make();
        const command = testEditHarness.transportCommand(testTransportPlayer);
        keyPress(Qt.Key_Space);
        verify(controller.spaceHeld);
        mousePress(item, 200, 6);
        mouseMove(item, 300, 6);
        verify(viewport.startUs !== 20000000);
        keyRelease(Qt.Key_Space);
        const stopped = viewport.startUs;
        mouseMove(item, 400, 6);
        compare(viewport.startUs, stopped);
        mouseRelease(item, 400, 6);
        compare(testEditHarness.transportCommand(testTransportPlayer), command);
    }
}
