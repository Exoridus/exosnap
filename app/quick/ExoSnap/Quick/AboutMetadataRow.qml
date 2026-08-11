pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property string label
    required property string value
    property bool linkEnabled: false

    signal linkActivated

    implicitHeight: 40

    RowLayout {
        anchors.fill: parent
        spacing: ExoTheme.spacingMd

        Label {
            text: root.label
            textFormat: Text.PlainText
            color: ExoTheme.textDim
            Layout.preferredWidth: 96
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontCaption
                capitalization: Font.AllUppercase
            }
        }

        Loader {
            active: root.linkEnabled
            Layout.fillWidth: true
            sourceComponent: ExoButton {
                width: parent ? parent.width : implicitWidth
                text: root.value
                quiet: true
                onClicked: root.linkActivated()
            }
        }

        Label {
            text: root.value
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: ExoTheme.textSecondary
            visible: !root.linkEnabled
            Layout.fillWidth: true
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontSecondary
                weight: Font.Medium
            }
        }
    }

    Rectangle {
        height: 1
        color: ExoTheme.line
        anchors {
            right: parent.right
            bottom: parent.bottom
            left: parent.left
        }
    }
}
