import QtQuick
import QtQuick.Controls

ExoButton {
    id: root
    property string shortcutText: ""
    compact: true
    Accessible.name: text
    ToolTip.text: shortcutText.length > 0 ? qsTr("%1 (%2)").arg(text).arg(shortcutText) : text
    ToolTip.visible: hovered || visualFocus
    ToolTip.delay: hovered ? 500 : 0
}
