import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Chevron collapsible: a header button plus a body that only exists while open.
// The body is a Loader so a collapsed section (self-test rows, the config table,
// an issue card's evidence) costs nothing until it is opened.
ColumnLayout {
    id: root

    required property string title
    property string subtitle: ""
    property bool expanded: false
    property alias body: bodyLoader.sourceComponent

    spacing: ExoTheme.spacingXs

    AbstractButton {
        id: header

        implicitHeight: 28
        hoverEnabled: true
        Layout.fillWidth: true
        Accessible.role: Accessible.Button
        Accessible.name: root.title
        onClicked: root.expanded = !root.expanded

        background: Rectangle {
            color: header.hovered ? ExoTheme.surfaceHover : "transparent"
            radius: ExoTheme.radiusSm
        }

        contentItem: RowLayout {
            spacing: ExoTheme.spacingSm

            ExoChevron {
                direction: root.expanded ? 0 : -90
                tone: header.hovered ? ExoTheme.text : ExoTheme.textMuted
                Layout.preferredWidth: 12
                Layout.alignment: Qt.AlignVCenter

                Behavior on rotation {
                    NumberAnimation {
                        duration: ExoTheme.animMedium
                        easing.type: Easing.OutCubic
                    }
                }
            }

            Label {
                text: root.title
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.textSecondary
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                    weight: Font.DemiBold
                }
            }
        }
    }

    Label {
        text: root.subtitle
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        visible: root.expanded && root.subtitle !== ""
        color: ExoTheme.textMuted
        Layout.fillWidth: true
        Layout.leftMargin: ExoTheme.spacingLg
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontCaption
        }
    }

    Loader {
        id: bodyLoader

        active: root.expanded
        visible: root.expanded
        Layout.fillWidth: true
        Layout.leftMargin: ExoTheme.spacingLg
    }
}
