pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: root
    required property EditSessionAdapter session
    required property var applyValue
    required property var parseValue
    required property var formatValue
    property var targetId: 0
    property real value: 0
    property var context: null
    property Item canvasFocusItem: null
    property string errorText: ""
    readonly property bool draftActive: editing
    property bool editing: false
    property bool applying: false
    property var capturedTarget: 0
    property real capturedValue: 0
    property var capturedContext: null
    property string capturedText: ""

    implicitHeight: ExoTheme.controlHeight
    selectByMouse: true
    maximumLength: 128
    font.family: ExoTheme.monoFamily
    font.pixelSize: ExoTheme.fontBody
    color: enabled ? ExoTheme.text : ExoTheme.textSecondary
    selectionColor: ExoTheme.accent
    selectedTextColor: ExoTheme.accentInk
    leftPadding: ExoTheme.spacingMd
    rightPadding: ExoTheme.spacingMd
    Accessible.description: errorText
    ToolTip.visible: errorText.length > 0 && (hovered || activeFocus)
    ToolTip.text: errorText

    function adoptValue(): void {
        if (session && !editing && typeof formatValue === "function")
            text = formatValue(value, context);
    }
    function beginDraft(): void {
        if (editing || !enabled || !session)
            return;
        capturedTarget = targetId;
        capturedValue = value;
        capturedContext = context;
        capturedText = formatValue(capturedValue, capturedContext);
        editing = true;
        errorText = "";
        session.setPendingDraftOwner(root, capturedTarget);
    }
    function cancelPendingDraft(reason: string): void {
        editing = false;
        if (session)
            session.clearPendingDraftOwner(root);
        errorText = reason;
        adoptValue();
    }
    function commitPendingDraft(): bool {
        if (!editing || applying)
            return true;
        if (text === capturedText) {
            cancelPendingDraft("");
            return true;
        }
        const parsed = parseValue(text, capturedValue, capturedContext);
        if (!parsed.valid) {
            errorText = parsed.error;
            forceActiveFocus(Qt.OtherFocusReason);
            return false;
        }
        applying = true;
        const accepted = applyValue(parsed.value, capturedTarget, capturedContext);
        applying = false;
        if (!accepted) {
            errorText = session.workspaceError.length > 0 ? session.workspaceError : qsTr("This value is not valid for the selected item.");
            forceActiveFocus(Qt.OtherFocusReason);
            return false;
        }
        cancelPendingDraft("");
        return true;
    }
    function focusCanvas(): void {
        if (canvasFocusItem)
            canvasFocusItem.forceActiveFocus(Qt.OtherFocusReason);
    }
    onActiveFocusChanged: {
        if (activeFocus)
            beginDraft();
        else if (editing)
            commitPendingDraft();
    }
    onEditingFinished: commitPendingDraft()
    onTextEdited: if (!editing) beginDraft()
    onValueChanged: adoptValue()
    onContextChanged: adoptValue()
    onVisibleChanged: {
        if (!visible && editing)
            cancelPendingDraft(qsTr("The edited item is no longer available."));
    }
    onEnabledChanged: {
        if (!enabled && editing)
            cancelPendingDraft(qsTr("The edited item is no longer editable."));
    }
    onTargetIdChanged: {
        if (editing && !applying)
            cancelPendingDraft(qsTr("The edited item is no longer available."));
        adoptValue();
    }
    Component.onCompleted: adoptValue()
    Component.onDestruction: if (session) session.clearPendingDraftOwner(root)
    Keys.priority: Keys.BeforeItem
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (commitPendingDraft())
                focusCanvas();
            event.accepted = true;
        } else if (event.key === Qt.Key_Escape) {
            cancelPendingDraft("");
            focusCanvas();
            event.accepted = true;
        } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            event.accepted = !commitPendingDraft();
        }
    }
    background: Rectangle {
        radius: ExoTheme.radiusSm
        color: root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: 1
        border.color: root.errorText.length > 0 ? ExoTheme.error : root.activeFocus ? ExoTheme.accent : ExoTheme.line
    }
}
