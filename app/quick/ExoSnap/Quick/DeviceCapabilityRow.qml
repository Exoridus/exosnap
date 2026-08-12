pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One capability-matrix row: label left, evidence right. The evidence is either
// a mono value string or a list of per-codec chips — never both, and which one
// applies is decided by the adapter.
Item {
    id: root

    required property string label
    required property string valueText
    required property var chips
    required property bool firstRow

    implicitHeight: rowLayout.implicitHeight + 2 * ExoTheme.spacingSm

    Rectangle {
        height: 1
        color: ExoTheme.line
        visible: !root.firstRow
        anchors {
            top: parent.top
            right: parent.right
            left: parent.left
        }
    }

    RowLayout {
        id: rowLayout

        spacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            topMargin: ExoTheme.spacingSm
            bottomMargin: ExoTheme.spacingSm
        }

        Label {
            text: root.label
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.textSecondary
            Layout.minimumWidth: 120
            Layout.maximumWidth: 200
            Layout.alignment: Qt.AlignVCenter
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        Item {
            Layout.fillWidth: true
        }

        Label {
            text: root.valueText
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignRight
            visible: root.valueText !== ""
            color: ExoTheme.text
            Layout.alignment: Qt.AlignVCenter
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        Row {
            spacing: ExoTheme.spacingXs
            visible: root.chips.length > 0
            Layout.alignment: Qt.AlignVCenter

            Repeater {
                model: root.chips

                DeviceCodecChip {
                    required property var modelData

                    label: modelData.label
                    available: modelData.available
                }
            }
        }
    }
}
