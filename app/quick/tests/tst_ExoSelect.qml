import QtQuick
import QtQuick.Controls.Basic
import QtTest
import ExoSnap.Quick.TestControls

TestCase {
    id: testCase
    name: "ExoSelect"
    when: windowShown
    width: 520
    height: 500
    visible: true

    Component {
        id: selectComponent
        ExoSelect {
            id: testSelect
            width: 220
            value: 1
            property int activations: 0
            options: [
                {value: 1, label: "Available", selectable: true, reason: ""},
                {value: 2, label: "Unavailable", selectable: false,
                 reason: "Faster than the fastest attached display. This entire explanation remains readable at a narrow window width."},
                {value: 3, label: "Another choice", selectable: true, reason: ""}
            ]
            onValueActivated: value => { activations += 1; testSelect.value = value; }
        }
    }

    function test_unavailable_reason_wraps_and_choice_cannot_activate() {
        const select = createTemporaryObject(selectComponent, testCase);
        verify(!!select);
        select.forceActiveFocus();
        keyClick(Qt.Key_Space);
        tryCompare(select.popup, "opened", true);
        const reason = findChild(select.popup.contentItem, "selectOptionReason");
        verify(!!reason);
        // The first instantiated row has no reason; locate the visible one.
        let shown = null;
        for (const delegate of select.popup.contentItem.contentItem.children) {
            const label = findChild(delegate, "selectOptionReason");
            if (label && label.visible)
                shown = label;
        }
        verify(!!shown);
        compare(shown.truncated, false);
        verify(shown.lineCount > 1);
        verify(select.popup.width > select.width);
        mouseClick(shown);
        compare(select.activations, 0);
        compare(select.value, 1);
        select.forceActiveFocus();
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        compare(select.value, 1);
        compare(select.activations, 0);
        keyClick(Qt.Key_Space);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        compare(select.value, 3);
        compare(select.activations, 1);
    }

    function test_keyboard_can_scroll_all_unavailable_explanations() {
        const options = [{value: 1, label: "Available", selectable: true, reason: ""}];
        for (let i = 2; i < 12; ++i)
            options.push({value: i, label: "Unavailable " + i, selectable: false,
                          reason: "A full explanation is visible below the option label and can be read by scrolling the open list."});
        const select = createTemporaryObject(selectComponent, testCase, {options: options});
        select.forceActiveFocus();
        keyClick(Qt.Key_Space);
        tryCompare(select.popup, "opened", true);
        const list = select.popup.contentItem;
        compare(list.contentY, 0);
        keyClick(Qt.Key_PageDown);
        verify(list.contentY > 0);
        keyClick(Qt.Key_PageUp);
        compare(list.contentY, 0);
        compare(select.activations, 0);
    }
}
