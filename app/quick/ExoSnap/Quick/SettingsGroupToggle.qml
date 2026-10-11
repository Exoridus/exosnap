import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The "Configure / Hide" control of a settings row that opens a group of further
// rows directly beneath it, at the same indent and rhythm as the rest of the
// card. Unlike ExoDisclosure it owns no body: the host toggles the group's
// visibility from `open`, so the group's rows stay ordinary card rows instead of
// a nested panel with its own padding.
Item {
    id: root

    property bool open: false
    property string accessibleName: ""

    implicitWidth: toggleRow.implicitWidth
    implicitHeight: toggleRow.implicitHeight
    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

    // Same pattern as ExoDisclosure's header: a plain Item stands in for a
    // button, so Tab order, Space activation and the Accessible role that a real
    // Control gets for free all need to be spelled out here. Space only, not
    // Enter - product-spec 10.1 reserves Enter for a dialog's default button and
    // gives every other control exactly one activation key.
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: root.accessibleName
    Accessible.checkable: true
    Accessible.checked: root.open
    Accessible.focusable: true
    Accessible.focused: root.activeFocus
    Accessible.onPressAction: root.toggle()
    Accessible.onToggleAction: root.toggle()

    function toggle(): void {
        root.open = !root.open;
    }

    // Harness seam (--visual-expand-all): the open state is not a property the
    // harness knows by name.
    function harnessExpand(): void {
        root.open = true;
    }

    Keys.onSpacePressed: event => {
        root.toggle();
        event.accepted = true;
    }

    Rectangle {
        anchors.fill: parent
        radius: ExoTheme.radiusSm
        color: "transparent"
        border.width: root.activeFocus ? ExoTheme.focusRingWidth : 0
        border.color: ExoTheme.text
    }

    RowLayout {
        id: toggleRow

        anchors.fill: parent
        spacing: ExoTheme.spacingSm

        Label {
            text: root.open ? qsTr("Hide") : qsTr("Configure")
            textFormat: Text.PlainText
            color: ExoTheme.textSecondary
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        ExoChevron {
            // 0 = down (expanded), -90 = right (collapsed) - same convention as
            // ExoDisclosure's header chevron.
            direction: root.open ? 0 : -90
            tone: ExoTheme.textMuted
            Layout.preferredWidth: 12

            Behavior on rotation {
                NumberAnimation {
                    duration: ExoTheme.animMedium
                    easing.type: Easing.OutCubic
                }
            }
        }
    }

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }

    TapHandler {
        onTapped: root.toggle()
    }
}
