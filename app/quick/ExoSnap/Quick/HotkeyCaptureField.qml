import QtQuick
import QtQuick.Controls.Basic

// A shortcut field. Focusing it starts capturing, so nothing typed lands in it:
// held modifiers show as key caps, and the first other key commits the
// combination as raw key and modifier bits. Whether that combination is valid,
// reserved or already taken is decided by GlobalHotkeyService, never here.
//
// Tab and Shift+Tab without Ctrl, Alt or Windows still move focus, so the field
// is never a keyboard trap. Escape drops the held modifiers.
Control {
    id: root

    // Key caps of the current binding, e.g. ["Ctrl", "Shift", "R"].
    property var keys: []
    // The same binding as one string, for assistive technology.
    property string bindingText: ""
    // Why the last attempt was refused; errorKeys then shows that attempt.
    property string errorText: ""
    property var errorKeys: []
    // Localized Ctrl, Alt, Shift and Windows names, in that order.
    property var modifierNames: []
    property string clearAccessibleName: ""

    readonly property bool hasError: root.errorText !== ""
    readonly property bool capturing: root.activeFocus
    readonly property Item clearButton: clearAction
    property int heldModifiers: 0

    signal captureStarted()
    signal captureCancelled()
    signal captured(int key, int modifiers)
    signal cleared()
    signal errorDismissed()

    readonly property var shownKeys: {
        if (root.hasError)
            return root.errorKeys;
        if (root.capturing)
            return root.modifierCaps(root.heldModifiers);
        return root.keys;
    }

    function modifierCaps(modifiers: int): var {
        const bits = [Qt.ControlModifier, Qt.AltModifier, Qt.ShiftModifier, Qt.MetaModifier];
        let caps = [];
        for (let i = 0; i < bits.length; ++i) {
            if ((modifiers & bits[i]) && i < root.modifierNames.length)
                caps.push(root.modifierNames[i]);
        }
        return caps;
    }

    function modifierBit(key: int): int {
        switch (key) {
        case Qt.Key_Control: return Qt.ControlModifier;
        case Qt.Key_Alt: return Qt.AltModifier;
        case Qt.Key_Shift: return Qt.ShiftModifier;
        case Qt.Key_Meta: return Qt.MetaModifier;
        default: return 0;
        }
    }

    implicitHeight: ExoTheme.controlHeight
    implicitWidth: 200
    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: true
    hoverEnabled: true
    leftPadding: ExoTheme.spacingSm
    rightPadding: (clearAction.visible ? clearAction.width + 4 : ExoTheme.spacingSm)
                  + (root.hasError ? errorMark.width + ExoTheme.spacingXs : 0)
    ToolTip.visible: root.hasError && (root.hovered || root.activeFocus || clearAction.activeFocus)
    ToolTip.text: root.errorText
    ToolTip.delay: 200
    Accessible.role: Accessible.EditableText
    Accessible.description: root.hasError ? root.errorText
                          : root.capturing ? qsTr("Press a key combination")
                          : root.bindingText !== "" ? root.bindingText : qsTr("Not set")

    onActiveFocusChanged: {
        root.heldModifiers = 0;
        if (root.activeFocus)
            root.captureStarted();
        else
            root.captureCancelled();
    }

    Keys.onPressed: event => {
        const chord = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.ShiftModifier | Qt.MetaModifier);
        if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                && (chord & ~Qt.ShiftModifier) === 0) {
            event.accepted = false;
            return;
        }
        event.accepted = true;
        const bit = root.modifierBit(event.key);
        if (bit !== 0) {
            root.heldModifiers = chord | bit;
            return;
        }
        if (event.key === Qt.Key_Escape && chord === 0) {
            root.heldModifiers = 0;
            return;
        }
        if (event.isAutoRepeat || event.key === Qt.Key_unknown)
            return;
        root.heldModifiers = 0;
        root.captured(event.key, chord);
        // The capture is over either way: on success the field shows the new
        // binding, on refusal the reason. The clear button is the one next
        // step for both, and moving focus off the field ends the capture.
        if (clearAction.visible)
            clearAction.forceActiveFocus(Qt.TabFocusReason);
    }
    Keys.onReleased: event => {
        const bit = root.modifierBit(event.key);
        if (bit !== 0)
            root.heldModifiers = root.heldModifiers & ~bit;
        event.accepted = true;
    }

    background: Rectangle {
        color: root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: 1
        border.color: root.hasError ? ExoTheme.error : root.activeFocus ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
    }

    contentItem: Item {
        implicitHeight: caps.implicitHeight

        Row {
            id: caps

            spacing: 4
            visible: root.shownKeys.length > 0
            anchors.verticalCenter: parent.verticalCenter

            Repeater {
                model: root.shownKeys

                Rectangle {
                    id: cap

                    required property string modelData

                    implicitWidth: Math.max(capLabel.implicitWidth + 2 * ExoTheme.spacingXs, height)
                    implicitHeight: 20
                    radius: ExoTheme.radiusXs
                    color: root.enabled ? ExoTheme.surface : "transparent"
                    border.width: 1
                    border.color: root.hasError ? ExoTheme.error : ExoTheme.line

                    Text {
                        id: capLabel

                        text: cap.modelData
                        textFormat: Text.PlainText
                        anchors.centerIn: parent
                        color: root.enabled ? ExoTheme.text : ExoTheme.textSecondary
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontCaption
                            weight: Font.DemiBold
                        }
                    }
                }
            }
        }

        Text {
            visible: root.shownKeys.length === 0
            text: root.capturing ? qsTr("Press a key combination") : qsTr("Not set")
            textFormat: Text.PlainText
            elide: Text.ElideRight
            width: parent.width
            anchors.verticalCenter: parent.verticalCenter
            color: root.capturing ? ExoTheme.textMuted : ExoTheme.textDim
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontBody
            }
        }
    }

    TapHandler {
        onTapped: root.forceActiveFocus(Qt.MouseFocusReason)
    }

    ExoFieldErrorMark {
        id: errorMark

        visible: root.hasError
        anchors.right: clearAction.visible ? clearAction.left : parent.right
        anchors.rightMargin: clearAction.visible ? ExoTheme.spacingXs : ExoTheme.spacingSm
        anchors.verticalCenter: parent.verticalCenter
    }

    AbstractButton {
        id: clearAction

        visible: root.hasError || root.keys.length > 0
        width: 28
        height: root.height - 8
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        enabled: root.enabled
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.Button
        Accessible.name: root.clearAccessibleName
        onClicked: {
            if (root.hasError)
                root.errorDismissed();
            else
                root.cleared();
            // Ready for the next combination straight away.
            root.forceActiveFocus(Qt.OtherFocusReason);
        }

        HoverHandler {
            cursorShape: clearAction.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        }

        background: Rectangle {
            radius: ExoTheme.radiusSm
            color: clearAction.hovered ? ExoTheme.surfaceHover : "transparent"
            border.width: clearAction.visualFocus ? ExoTheme.focusRingWidth : 0
            border.color: ExoTheme.text
        }

        contentItem: Item {
            ExoGlyph {
                kind: ExoGlyph.Close
                color: clearAction.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
                width: 12
                height: 12
                anchors.centerIn: parent
            }
        }
    }
}
