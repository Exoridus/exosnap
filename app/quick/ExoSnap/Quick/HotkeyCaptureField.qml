import QtQuick
import QtQuick.Controls.Basic

// Press-to-bind field. It only reports the raw key and modifier bits; whether
// the combination is valid, blocked, or already taken is decided by
// GlobalHotkeyService, never here.
Control {
    id: root

    required property bool capturing
    required property string binding

    signal captureRequested()
    signal captureCancelled()
    signal captured(int key, int modifiers)

    implicitHeight: ExoTheme.controlHeight
    implicitWidth: 160
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.Button

    onCapturingChanged: {
        if (root.capturing) {
            root.forceActiveFocus();
        }
    }

    Keys.onPressed: event => {
        if (!root.capturing) {
            // Not capturing yet, so this is the keyboard asking to START. Without
            // it the field was reachable by Tab and did nothing there: a bare
            // Control has none of AbstractButton's Enter/Space activation, and the
            // only path to captureRequested() was the TapHandler. No hotkey could
            // be rebound without a mouse.
            if (root.enabled && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                 || event.key === Qt.Key_Space)) {
                event.accepted = true;
                root.captureRequested();
            }
            return;
        }
        event.accepted = true;
        if (event.key === Qt.Key_Escape) {
            root.captureCancelled();
            return;
        }
        // Modifier-only presses are not a binding yet -- keep listening.
        if (event.key === Qt.Key_Shift || event.key === Qt.Key_Control || event.key === Qt.Key_Alt
                || event.key === Qt.Key_Meta) {
            return;
        }
        root.captured(event.key, event.modifiers);
    }

    contentItem: Label {
        text: root.capturing ? qsTr("Press a key combination…")
                             : root.binding === "" ? qsTr("Not set") : root.binding
        textFormat: Text.PlainText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignHCenter
        color: !root.enabled ? ExoTheme.textDim
                             : root.capturing ? ExoTheme.accent
                                              : root.binding === "" ? ExoTheme.textMuted : ExoTheme.text
        font {
            family: root.binding === "" || root.capturing ? ExoTheme.sansFamily : ExoTheme.monoFamily
            pixelSize: ExoTheme.fontSecondary
        }
    }

    background: Rectangle {
        color: root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: root.capturing || root.activeFocus ? 1 : 1
        border.color: root.capturing ? ExoTheme.accent : root.activeFocus ? ExoTheme.lineStrong : ExoTheme.line
        radius: ExoTheme.radiusSm
    }

    TapHandler {
        enabled: root.enabled && !root.capturing

        onTapped: root.captureRequested()
    }
}
