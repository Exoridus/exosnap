pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

EditDraftField {
    id: root
    property alias valueUs: root.value
    property int format: 0
    property bool sampleContext: false
    readonly property int effectiveFormat: root.format === 0 && root.session ? root.session.defaultTimeFormat : root.format
    readonly property string formatDescription: root.effectiveFormat === 0 ? qsTr("Non-drop-frame timecode") : root.effectiveFormat === 1 ? qsTr("Seconds, up to six decimal places") : root.effectiveFormat === 2 ? qsTr("48 kHz sample count") : qsTr("Minutes and seconds with milliseconds")
    context: ({format: root.effectiveFormat, samples: root.sampleContext})
    formatValue: (value, context) => root.session.formatTimeValue(value, context?.format ?? root.effectiveFormat)
    parseValue: (text, base, context) => {
        const parsed = root.session.parseTimeValue(text, base, context?.samples ?? root.sampleContext, context?.format ?? root.effectiveFormat);
        return {valid: parsed.valid, value: parsed.valueUs, error: parsed.error};
    }
    Accessible.description: errorText.length > 0 ? errorText : formatDescription

    function changeFormat(value: int): void {
        if (!commitPendingDraft())
            return;
        format = value;
        adoptValue();
    }
    component FormatOption: ExoMenuItem {
        id: option
        required property int mode
        checkable: true
        checked: root.effectiveFormat === mode
        leftPadding: 32
        onTriggered: root.changeFormat(mode)
        indicator: ExoGlyph {
            x: 10
            y: (option.height - height) / 2
            width: 14; height: 14
            kind: ExoGlyph.Check
            color: ExoTheme.accent
            visible: option.checked
        }
    }
    ContextMenu.menu: ExoMenu {
        onClosed: if (root.visible && root.enabled) root.forceActiveFocus(Qt.PopupFocusReason)
        FormatOption { mode: 0; text: qsTr("Timecode (non-drop-frame)"); enabled: root.session && root.session.defaultTimeFormat === 0 }
        FormatOption { mode: 3; text: qsTr("Minutes and seconds (milliseconds)") }
        FormatOption { mode: 1; text: qsTr("Seconds") }
        FormatOption { mode: 2; text: qsTr("Samples (48 kHz)"); visible: root.sampleContext }
        ExoMenuItem { text: qsTr("Cut"); enabled: root.selectedText.length > 0; onTriggered: root.cut() }
        ExoMenuItem { text: qsTr("Copy"); enabled: root.selectedText.length > 0; onTriggered: root.copy() }
        ExoMenuItem { text: qsTr("Paste"); enabled: root.canPaste; onTriggered: root.paste() }
        ExoMenuItem { text: qsTr("Select all"); onTriggered: root.selectAll() }
    }
}
