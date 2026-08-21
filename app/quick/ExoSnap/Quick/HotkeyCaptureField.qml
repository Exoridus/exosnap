import QtQuick
import QtQuick.Controls.Basic

// Passive binding display, driven by the row's Set/Change button rather than
// interactive itself. It only reports the raw key and modifier bits; whether
// the combination is valid, blocked, or already taken is decided by
// GlobalHotkeyService, never here.
Control {
    id: root

    required property bool capturing
    required property string binding

    signal captureCancelled()
    signal captured(int key, int modifiers)

    implicitHeight: ExoTheme.controlHeight
    implicitWidth: 160
    Accessible.role: Accessible.StaticText

    // No focusPolicy: at rest this is a label, not a tab stop or a click
    // target. It still receives key events while capturing because the
    // Set/Change button calls forceActiveFocus() on it directly -- that call
    // is unaffected by focusPolicy (Qt 6.7 Item docs: focusPolicy governs only
    // focus-by-click and focus-by-tab, not a programmatic focus request), so
    // Keys.onPressed below fires normally once capture starts.
    Keys.onPressed: event => {
        if (!root.capturing)
            return;
        event.accepted = true;
        if (event.key === Qt.Key_Escape) {
            root.captureCancelled();
            return;
        }
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

    // Chromed at rest, not only while capturing: the binding is a value the eye
    // has to find on a row that also carries two buttons, and as bare text it
    // reads as prose rather than as the shortcut this row is about.
    background: Rectangle {
        color: root.enabled ? ExoTheme.surfaceRaised : "transparent"
        border.width: 1
        border.color: root.capturing && root.enabled ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
    }
}
