pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Exclusive segmented selector (Logs severity: All / Info / Issues).
Rectangle {
    id: root

    required property var options
    property int currentIndex: 0

    signal selected(int index)

    implicitWidth: row.implicitWidth + 6
    implicitHeight: 30
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusSm

    RowLayout {
        id: row

        spacing: 0
        anchors {
            fill: parent
            margins: 3
        }

        Repeater {
            model: root.options

            AbstractButton {
                id: segment

                required property int index
                required property string modelData

                implicitWidth: Math.max(56, segmentLabel.implicitWidth + 2 * ExoTheme.spacingMd)
                hoverEnabled: true
                Layout.fillHeight: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: segment.modelData
                Accessible.checked: root.currentIndex === segment.index
                onClicked: root.selected(segment.index)

                background: Rectangle {
                    color: root.currentIndex === segment.index ? ExoTheme.surfaceHover
                         : segment.hovered ? ExoTheme.surfaceRaised
                         : "transparent"
                    border.width: root.currentIndex === segment.index ? 1 : 0
                    border.color: ExoTheme.accent
                    radius: ExoTheme.radiusXs
                }

                contentItem: Label {
                    id: segmentLabel

                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: segment.modelData
                    textFormat: Text.PlainText
                    color: root.currentIndex === segment.index ? ExoTheme.text : ExoTheme.textSecondary
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontSecondary
                        weight: root.currentIndex === segment.index ? Font.DemiBold : Font.Medium
                    }
                }
            }
        }
    }
}
