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

    function test_closed_keyboard_traverses_available_options_data() {
        return [
            {tag: "forward", initial: 1, key: Qt.Key_Down, expected: 3, activations: 1},
            {tag: "backward", initial: 3, key: Qt.Key_Up, expected: 1, activations: 1},
            {tag: "first boundary", initial: 1, key: Qt.Key_Up, expected: 1, activations: 0},
            {tag: "last boundary", initial: 3, key: Qt.Key_Down, expected: 3, activations: 0},
            {tag: "unavailable current forward", initial: 2, key: Qt.Key_Down, expected: 3, activations: 1},
            {tag: "unavailable current backward", initial: 2, key: Qt.Key_Up, expected: 1, activations: 1},
            {tag: "unmatched current forward", initial: -1, key: Qt.Key_Down, expected: 1, activations: 1},
            {tag: "unmatched current backward", initial: -1, key: Qt.Key_Up, expected: 3, activations: 1}
        ];
    }

    function test_closed_keyboard_traverses_available_options(data) {
        const select = createTemporaryObject(selectComponent, testCase, {value: data.initial});
        verify(!!select);
        select.forceActiveFocus();
        keyClick(data.key);
        compare(select.popup.visible, false);
        compare(select.value, data.expected);
        compare(select.activations, data.activations);
        if (data.expected === 1 || data.expected === 3) {
            // A second press at the reached boundary cannot activate again.
            const boundaryKey = data.expected === 1 ? Qt.Key_Up : Qt.Key_Down;
            keyClick(boundaryKey);
            compare(select.value, data.expected);
            compare(select.activations, data.activations);
        }
    }

    function test_closed_keyboard_preserves_value_when_no_option_is_available_data() {
        return [
            {tag: "all unavailable", options: [
                {value: 1, label: "One", selectable: false, reason: "Unavailable"},
                {value: 2, label: "Two", selectable: false, reason: "Unavailable"}
            ]},
            {tag: "empty", options: []}
        ];
    }

    function test_closed_keyboard_preserves_value_when_no_option_is_available(data) {
        const select = createTemporaryObject(selectComponent, testCase, {options: data.options});
        select.forceActiveFocus();
        for (const key of [Qt.Key_Down, Qt.Key_Down, Qt.Key_Up, Qt.Key_Up]) {
            keyClick(key);
            compare(select.value, 1);
            compare(select.activations, 0);
            compare(select.popup.visible, false);
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
