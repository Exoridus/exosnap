import QtQuick
import QtQuick.Controls

ExoButton {
    id: root
    property string shortcutText: ""
    property bool panelAccess: false
    property bool expanded: false
    property bool pinned: false
    compact: true
    quiet: tone !== "primary"
    implicitWidth: 36
    implicitHeight: 32
    Accessible.name: text
    Accessible.checkable: root.checkable
    Accessible.checked: root.checked
    Accessible.expandable: panelAccess
    Accessible.expanded: panelAccess && expanded
    Accessible.description: panelAccess ? (pinned ? qsTr("Pinned panel") : expanded ? qsTr("Open panel") : qsTr("Closed panel")) : ""
    ToolTip.text: shortcutText.length > 0 ? qsTr("%1 (%2)").arg(text).arg(shortcutText) : text
    ToolTip.visible: hovered || visualFocus
    ToolTip.delay: hovered ? 500 : 0
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: root.panelAccess ? 6 : 16
        height: root.panelAccess ? 6 : 2
        radius: root.panelAccess ? 3 : 1
        color: root.panelAccess && root.pinned ? ExoTheme.text : ExoTheme.accent
        visible: root.panelAccess ? root.expanded : root.checkable && root.checked
        Accessible.ignored: true
    }
}
