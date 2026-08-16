pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root

    required property bool frameReady
    required property real presentationRate
    required property real sourceDeliveryRate
    required property real frameTimeP95Ms
    required property real frameTimeP99Ms
    required property color accentColor
    required property color surfaceColor
    required property color textColor
    required property color secondaryTextColor
    required property string sansFamily
    required property string monoFamily
    property bool expanded: false

    signal toggled(bool expanded)

    Button {
        id: overlayButton

        objectName: "previewOverlayButton"
        width: Math.min(implicitWidth, Math.max(0, parent.width))
        text: root.expanded ? qsTr("Hide scene metrics") : qsTr("Show scene metrics")
        padding: 10
        onClicked: root.toggled(!root.expanded)
        anchors {
            top: parent.top
            right: parent.right
        }
        contentItem: Text {
            text: overlayButton.text
            color: root.textColor
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            font {
                family: root.sansFamily
                pixelSize: 11
                weight: Font.DemiBold
            }
        }

        background: Rectangle {
            color: overlayButton.down ? Qt.darker(root.surfaceColor, 1.18) : root.surfaceColor
            border.width: 1
            border.color: root.frameReady ? root.accentColor : root.secondaryTextColor
            radius: 5
        }
    }

    Rectangle {
        objectName: "previewMetricsOverlay"
        width: Math.min(220, Math.max(0, parent.width))
        height: metricsColumn.implicitHeight + 20
        color: root.surfaceColor
        border.width: 1
        border.color: root.accentColor
        radius: 8
        visible: root.expanded
        anchors {
            top: overlayButton.bottom
            right: parent.right
            topMargin: 8
        }

        Column {
            id: metricsColumn

            spacing: 4
            anchors {
                right: parent.right
                left: parent.left
                verticalCenter: parent.verticalCenter
                margins: 10
            }

            Label {
                width: parent.width
                text: qsTr("QML OVERLAY · NOT IN FRAME")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: root.accentColor
                font {
                    family: root.monoFamily
                    pixelSize: 9
                    weight: Font.DemiBold
                }
            }

            Label {
                width: parent.width
                text: qsTr("Delivery %1 · scene %2 fps")
                      .arg(root.sourceDeliveryRate.toFixed(1))
                      .arg(root.presentationRate.toFixed(1))
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: root.textColor
                font.family: root.monoFamily
                font.pixelSize: 11
            }

            Label {
                width: parent.width
                text: qsTr("p95 / p99  %1 / %2 ms")
                      .arg(root.frameTimeP95Ms.toFixed(2))
                      .arg(root.frameTimeP99Ms.toFixed(2))
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: root.secondaryTextColor
                font.family: root.monoFamily
                font.pixelSize: 11
            }
        }
    }
}
