import QtQuick
import QtTest
import ExoSnap.Quick.RecordPickerTestControls

TestCase {
    id: testCase
    name: "AudioSourceDetailsPopup"
    when: windowShown
    width: 860
    height: 700
    visible: true

    Component {
        id: popupComponent
        AudioSourceDetailsPopup {
            routing: recordDriver.settings.audioRouting
        }
    }

    function init() {
        recordDriver.seedAudioSources(2, "Source ");
        recordDriver.clearActiveAudioPlan();
    }

    function findVisual(item, name) {
        if (item.objectName === name)
            return item;
        for (const child of item.children || []) {
            const found = findVisual(child, name);
            if (found)
                return found;
        }
        return null;
    }

    function test_source_parameters_accept_decimals_and_reset_to_engine_defaults() {
        const routing = recordDriver.settings.audioRouting;
        const popup = createTemporaryObject(popupComponent, testCase, {sourceId: routing.sources[0].id});
        verify(popup);
        popup.open();
        tryCompare(popup, "opened", true);
        const field = findVisual(popup.contentItem, "audioSourceParameter-gainDb");
        verify(field);
        compare(field.suffix, "dB");
        field.locale = Qt.locale("de_DE");
        field.forceActiveFocus();
        field.contentItem.selectAll();
        field.contentItem.remove(field.contentItem.selectionStart, field.contentItem.selectionEnd);
        field.contentItem.insert(0, "-3,125");
        keyClick(Qt.Key_Return);
        tryVerify(() => Math.abs(popup.source.gainDb + 3.125) < 0.001);
        field.resetRequested();
        tryVerify(() => Math.abs(popup.source.gainDb - routing.sourceDetailDefaults.gainDb) < 0.001);
        compare(routing.sources[1].gainDb, routing.sourceDetailDefaults.gainDb);
        popup.close();
        tryCompare(popup, "visible", false);
    }

    function test_source_parameter_error_stays_at_the_failed_field() {
        const routing = recordDriver.settings.audioRouting;
        const popup = createTemporaryObject(popupComponent, testCase, {sourceId: routing.sources[0].id});
        popup.open();
        tryCompare(popup, "opened", true);
        const field = findVisual(popup.contentItem, "audioSourceParameter-gateThresholdDb");
        verify(field);
        popup.patch("gateThresholdDb", 10);
        compare(popup.errorKey, "gateThresholdDb");
        verify(field.errorText.length > 0);
        compare(popup.source.gateThresholdDb, routing.sourceDetailDefaults.gateThresholdDb);
        popup.patch("gateThresholdDb", -40.5);
        compare(field.errorText, "");
        compare(popup.source.gateThresholdDb, -40.5);
        popup.close();
        tryCompare(popup, "visible", false);
    }

    function test_retained_gain_outside_editor_range_is_never_shown_as_clamped_value_data() {
        return [{tag: "silence", linear: 0, expected: Number.NEGATIVE_INFINITY},
                {tag: "above-editor-range", linear: 100, expected: 40}];
    }

    function test_failed_retained_value_operation_reports_on_the_visible_field() {
        recordDriver.seedAudioSources(1, "Retained ", 0);
        const routing = recordDriver.settings.audioRouting;
        const popup = createTemporaryObject(popupComponent, testCase, {sourceId: routing.sources[0].id});
        popup.open();
        tryCompare(popup, "opened", true);
        const retained = findVisual(popup.contentItem, "audioSourceRetainedParameter-gainDb");
        verify(retained.visible);
        popup.patch("gainDb", 1000);
        verify(retained.errorText.length > 0);
        compare(retained.Accessible.description, retained.errorText);
        compare(popup.source.gainDb, Number.NEGATIVE_INFINITY);
        popup.close();
        tryCompare(popup, "visible", false);
    }

    function test_retained_gain_outside_editor_range_is_never_shown_as_clamped_value(data) {
        recordDriver.seedAudioSources(1, "Retained ", data.linear);
        const routing = recordDriver.settings.audioRouting;
        const popup = createTemporaryObject(popupComponent, testCase, {sourceId: routing.sources[0].id});
        popup.open();
        tryCompare(popup, "opened", true);
        const revision = routing.revision;
        const field = findVisual(popup.contentItem, "audioSourceParameter-gainDb");
        const retained = findVisual(popup.contentItem, "audioSourceRetainedParameter-gainDb");
        verify(!field.visible);
        verify(retained.visible && retained.readOnly);
        compare(popup.source.gainDb, data.expected);
        verify(retained.value.includes(data.linear === 0 ? "−∞" : "40"));
        retained.forceActiveFocus();
        keyClick(Qt.Key_Tab);
        compare(routing.revision, revision);
        compare(popup.source.gainDb, data.expected);
        retained.trailingClicked();
        tryCompare(field, "visible", true);
        compare(popup.source.gainDb, routing.sourceDetailDefaults.gainDb);
        popup.close();
        tryCompare(popup, "visible", false);
    }
}
