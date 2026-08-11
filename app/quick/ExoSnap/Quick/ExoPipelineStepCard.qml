import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One pipeline stage. Status keys match diagnostics::StageStatusKey; an unmeasured
// value renders as an em dash, never as a zero.
Rectangle {
    id: root

    required property string title
    required property string status
    required property string lane
    required property string value
    property string tip: ""

    readonly property color statusColor: root.status === "over" ? ExoTheme.error
                                       : root.status === "hotspot" ? ExoTheme.warning
                                       : root.status === "ok" ? ExoTheme.success
                                       : ExoTheme.textDim

    implicitWidth: 150
    implicitHeight: column.implicitHeight + 2 * ExoTheme.spacingSm
    color: ExoTheme.surface
    border.width: 1
    border.color: root.status === "planned" || root.status === "unavailable" ? ExoTheme.line : root.statusColor
    radius: ExoTheme.radiusMd

    Accessible.role: Accessible.StaticText
    Accessible.name: root.title + ": " + root.value

    ToolTip.text: root.tip
    ToolTip.visible: root.tip !== "" && cardHover.hovered
    ToolTip.delay: 400

    HoverHandler {
        id: cardHover
    }

    ColumnLayout {
        id: column

        spacing: 2
        anchors {
            fill: parent
            margins: ExoTheme.spacingSm
        }

        RowLayout {
            spacing: ExoTheme.spacingXs
            Layout.fillWidth: true

            Rectangle {
                color: root.statusColor
                radius: 3
                Layout.preferredWidth: 6
                Layout.preferredHeight: 6
                Layout.alignment: Qt.AlignVCenter
            }

            Label {
                text: root.title
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.textSecondary
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: 11
                    weight: Font.DemiBold
                }
            }
        }

        Label {
            text: root.value
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: ExoTheme.text
            Layout.fillWidth: true
            font {
                family: ExoTheme.monoFamily
                pixelSize: 13
            }
        }

        Label {
            text: root.lane
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: ExoTheme.textDim
            Layout.fillWidth: true
            font {
                family: ExoTheme.monoFamily
                pixelSize: 9
                letterSpacing: 1
            }
        }
    }
}
