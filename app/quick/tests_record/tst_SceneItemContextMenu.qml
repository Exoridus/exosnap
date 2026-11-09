import QtQuick
import QtQuick.Controls.Basic
import QtTest
import ExoSnap.Quick.RecordPickerTestControls

TestCase {
    id: testCase
    name: "SceneItemContextMenu"
    when: windowShown
    width: 600
    height: 400
    visible: true

    Component {
        id: menuComponent
        SceneItemContextMenu {
            settings: recordDriver.settings
        }
    }

    SignalSpy {
        id: editSpy
        target: recordDriver.settings
        signalName: "configEdited"
    }

    function init() {
        recordDriver.resetVisualSettings(false, true);
        editSpy.clear();
    }
    function cleanup() {
        recordDriver.resetVisualSettings();
    }
    function padState() {
        return recordDriver.settings.visualSourceState(recordDriver.settings.recordGamepadSourceId, true);
    }
    function camState() {
        return recordDriver.settings.visualSourceState(recordDriver.settings.recordWebcamSourceId, true);
    }
    function openMenu(state, at) {
        const menu = createTemporaryObject(menuComponent, testCase);
        verify(!!menu);
        menu.openFor(state, at);
        tryCompare(menu, "opened", true);
        waitForRendering(menu.contentItem);
        return menu;
    }
    function rows(menu) {
        const result = [];
        for (let index = 0; index < menu.count; ++index) {
            const item = menu.itemAt(index);
            if (item && item.visible && item.height > 0)
                result.push(item);
        }
        return result;
    }
    function row(menu, objectName) {
        return rows(menu).find(item => item.objectName === objectName);
    }
    function verifyInside(popup, message) {
        const origin = popup.background.mapToItem(testCase, 0, 0);
        verify(origin.x >= 0 && origin.y >= 0, message + " starts inside the window");
        verify(origin.x + popup.width <= testCase.width + 0.5, message + " ends inside the window horizontally");
        verify(origin.y + popup.height <= testCase.height + 0.5, message + " ends inside the window vertically");
    }

    function test_controller_menu_is_one_vertical_column_with_host_rows_after_device() {
        const menu = openMenu(padState(), Qt.point(40, 40));
        const items = rows(menu);
        compare(items[0].text, "Input device", "the controller contributes only its device selection");
        verify(items[0].subMenu !== null);
        compare(items[0].valueText, "Automatic");
        const names = items.map(item => item.objectName).filter(name => name.length > 0);
        compare(names, ["sceneItemBringToFront", "sceneItemSendToBack", "sceneItemVisibility", "sceneItemRemove"]);
        for (let index = 1; index < items.length; ++index) {
            compare(items[index].x, items[0].x, "every row shares one column");
            verify(items[index].y >= items[index - 1].y + items[index - 1].height - 0.5, "rows stack downward");
        }
        verify(!items.some(item => item.text === "Layout"), "layout stays in the advanced settings");
    }

    function test_menu_and_submenu_stay_inside_the_window() {
        const menu = openMenu(padState(), Qt.point(testCase.width - 4, testCase.height - 4));
        verifyInside(menu, "menu");
        const device = rows(menu)[0];
        mouseClick(device);
        tryCompare(device.subMenu, "opened", true);
        verifyInside(device.subMenu, "submenu");
        compare(device.subMenu.count, 3);
    }

    // The webcam fixture's device ids are valid camera ids. Controller ids are
    // 64-digit device identities the fixture does not provide.
    function test_device_choice_dispatches_through_the_registered_action() {
        const menu = openMenu(camState(), Qt.point(40, 40));
        const device = rows(menu)[0];
        mouseClick(device);
        tryCompare(device.subMenu, "opened", true);
        const camera = device.subMenu.itemAt(1);
        compare(camera.text, "Camera");
        mouseClick(camera);
        tryCompare(editSpy, "count", 1);
        compare(recordDriver.settings.sceneItemMenuEntries(camState().sourceId, camState().itemId)[0].value, "camera");
    }

    function test_hide_then_show_and_remove_use_host_commands() {
        let menu = openMenu(padState(), Qt.point(40, 40));
        compare(row(menu, "sceneItemVisibility").text, "Hide from recording");
        mouseClick(row(menu, "sceneItemVisibility"));
        tryCompare(editSpy, "count", 1);
        compare(padState().outputVisible, false);
        menu = openMenu(padState(), Qt.point(40, 40));
        compare(row(menu, "sceneItemVisibility").text, "Show in recording");
        const cameraItem = camState().itemId;
        mouseClick(row(menu, "sceneItemRemove"));
        tryCompare(editSpy, "count", 2);
        compare(padState().itemId, "", "the controller item is gone");
        compare(camState().itemId, cameraItem, "the webcam item is untouched");
    }

    function test_stale_menu_is_rejected_after_the_item_was_removed() {
        const stale = padState();
        const menu = openMenu(stale, Qt.point(40, 40));
        verify(recordDriver.settings.applySceneItemCommand(stale.sourceId, stale.itemId, stale.runtime,
                                                           stale.editRevision, "remove"));
        verify(!menu.command("front"));
        verify(!menu.command("remove"));
        compare(editSpy.count, 1);
    }

    function test_stacking_rows_follow_the_scene_edges() {
        const menu = openMenu(padState(), Qt.point(40, 40));
        verify(!row(menu, "sceneItemBringToFront").enabled, "the controller starts on top");
        verify(row(menu, "sceneItemSendToBack").enabled);
        mouseClick(row(menu, "sceneItemSendToBack"));
        tryCompare(editSpy, "count", 1);
        compare(padState().stackIndex, 0);
        compare(camState().stackIndex, 1);
    }
}
