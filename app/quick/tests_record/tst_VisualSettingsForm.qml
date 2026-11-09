import QtQuick
import QtTest
import ExoSnap.Quick.RecordPickerTestControls

TestCase {
    id: testCase
    name: "VisualSettingsForm"
    when: windowShown
    width: 600
    height: 500
    visible: true

    Component {
        id: formComponent
        VisualSettingsForm {
            width: 480
            settings: recordDriver.settings
            stacked: true
        }
    }

    SignalSpy {
        id: editSpy
        target: recordDriver.settings
        signalName: "configEdited"
    }
    SignalSpy {
        id: retrySpy
        target: recordDriver.settings
        signalName: "visualRetryRequested"
    }
    SignalSpy {
        id: activeEditSpy
        target: recordDriver.settings
        signalName: "activeVisualSceneEdited"
    }

    function init() {
        recordDriver.resetVisualSettings();
        editSpy.clear();
        retrySpy.clear();
        activeEditSpy.clear();
    }
    function cleanup() {
        recordDriver.resetVisualSettings();
    }
    function findControl(item, label) {
        if ((item.valueActivated !== undefined && item.Accessible.name === label) ||
            (item.clicked !== undefined && item.text === label))
            return item;
        for (const child of item.children || []) {
            const found = findControl(child, label);
            if (found)
                return found;
        }
        return null;
    }
    function createForm(gamepad = false) {
        const form = createTemporaryObject(formComponent, testCase, {
            sourceId: gamepad ? recordDriver.settings.gamepadSourceId : recordDriver.settings.webcamSourceId
        });
        verify(!!form);
        waitForRendering(form);
        return form;
    }
    function test_cached_choices_render_without_editing_data() {
        return [{tag: "webcam", gamepad: false, rows: 3}, {tag: "gamepad", gamepad: true, rows: 3}];
    }
    function test_cached_choices_render_without_editing(data) {
        const form = createForm(data.gamepad);
        compare(form.rows.length, data.rows);
        compare(!!findControl(form, qsTr("Details")), !data.gamepad);
        verify(!!findControl(form, qsTr("Retry")));
        const select = findControl(form, qsTr("Device"));
        verify(!!select);
        compare(select.options.length, 3);
        compare(select.options[2].value, "blocked");
        compare(select.options[2].selectable, false);
        compare(select.value, "");
        compare(editSpy.count, 0);
    }
    function test_select_commits_through_the_form_data() {
        return [{tag: "webcam", gamepad: false, label: qsTr("Device"), value: "camera"},
                {tag: "gamepad", gamepad: true, label: qsTr("Layout"), value: "xbox_elite"}];
    }
    function test_select_commits_through_the_form(data) {
        const form = createForm(data.gamepad);
        const select = findControl(form, data.label);
        verify(!!select);
        select.forceActiveFocus();
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryCompare(editSpy, "count", 1);
        const expectedRow = data.gamepad ? 1 : 0;
        compare(recordDriver.settings.visualSettingsRows(form.sourceId)[expectedRow].value, data.value);
        const updated = findControl(form, data.label);
        verify(!!updated);
        tryCompare(updated, "value", data.value);
    }
    function test_recording_scope_locks_camera_and_rejects_stale_rows() {
        const form = createForm();
        const stale = form.rows[0];
        recordDriver.setRecordingContext(true);
        compare(recordDriver.settings.visualSourceState(form.sourceId).scope, "active");
        tryVerify(() => form.sourceState.scope === "active");
        const select = findControl(form, qsTr("Device"));
        verify(!!select);
        compare(select.enabled, false);
        verify(!recordDriver.settings.selectVisualSetting(stale.sourceId, stale.itemId, stale.editRevision,
                                                         stale.action, "camera"));
        compare(editSpy.count, 0);
        recordDriver.settings.editNextRecording();
        tryVerify(() => form.sourceState.scope === "editable");
        tryVerify(() => findControl(form, qsTr("Device")).enabled);
        compare(findControl(form, qsTr("Device")).value, "");
    }
    function test_read_only_scene_disables_selection() {
        recordDriver.resetVisualSettings(true);
        const form = createForm();
        const select = findControl(form, qsTr("Device"));
        verify(!!select);
        compare(select.enabled, false);
        compare(form.sourceState.editable, false);
        const row = form.rows[0];
        verify(!recordDriver.settings.selectVisualSetting(row.sourceId, row.itemId, row.editRevision,
                                                         row.action, "camera"));
        compare(editSpy.count, 0);
    }
    function test_retry_dispatch_requires_current_runtime() {
        const inactive = createForm();
        const inactiveRetry = findControl(inactive, qsTr("Retry"));
        verify(!!inactiveRetry);
        compare(inactiveRetry.enabled, false);
        inactive.destroy();
        recordDriver.resetVisualSettings(false, true);
        const active = createForm();
        const retry = findControl(active, qsTr("Retry"));
        verify(!!retry);
        compare(retry.enabled, true);
        mouseClick(retry);
        tryCompare(retrySpy, "count", 1);
        compare(editSpy.count, 0);
        const row = active.rows[2];
        recordDriver.resetVisualSettings();
        verify(!recordDriver.settings.dispatchVisualAction(row.sourceId, row.itemId, row.runtime, row.action));
        compare(retrySpy.count, 1);
    }
    function test_record_form_edits_active_gamepad_with_next_settings_open() {
        recordDriver.setRecordingContext(true);
        recordDriver.settings.editNextRecording();
        const next = createForm(true);
        const record = createTemporaryObject(formComponent, testCase, {
            sourceId: recordDriver.settings.recordGamepadSourceId,
            recordScope: true
        });
        verify(!!record);
        waitForRendering(record);
        compare(record.sourceState.scope, "active");
        compare(next.sourceState.scope, "editable");
        const layout = findControl(record, qsTr("Layout"));
        verify(!!layout);
        layout.forceActiveFocus();
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryCompare(activeEditSpy, "count", 1);
        compare(editSpy.count, 0);
        compare(findControl(record, qsTr("Layout")).value, "xbox_elite");
        compare(findControl(next, qsTr("Layout")).value, "auto");
        compare(recordDriver.settings.editingNextRecording, true);
    }
    function test_record_form_dispatches_runtime_action() {
        recordDriver.resetVisualSettings(false, true);
        const form = createTemporaryObject(formComponent, testCase, {
            sourceId: recordDriver.settings.recordGamepadSourceId,
            recordScope: true
        });
        verify(!!form);
        waitForRendering(form);
        const retry = findControl(form, qsTr("Retry"));
        verify(!!retry);
        verify(retry.enabled);
        mouseClick(retry);
        tryCompare(retrySpy, "count", 1);
    }
}
