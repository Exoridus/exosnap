import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// One DXGI adapter in the encoder-device grid. Selection is inspection only —
// it switches which capability matrix is shown below. The "ACTIVE" badge is
// independent of selection and belongs to whichever adapter actually backs the
// encoder, which only the adapter knows.
Button {
    id: root

    required property string title
    required property string kindBadge
    required property string backendLine
    required property bool active
    required property bool inspected

    implicitHeight: Math.max(62, cardRow.implicitHeight + 2 * ExoTheme.spacingMd)
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: root.title
    Accessible.description: root.active ? qsTr("Active encoder device") : root.backendLine

    contentItem: RowLayout {
        id: cardRow

        spacing: ExoTheme.spacingMd

        Rectangle {
            color: ExoTheme.surfaceRaised
            border.width: 1
            border.color: root.inspected ? ExoTheme.accent : ExoTheme.line
            radius: ExoTheme.radiusSm
            Layout.preferredWidth: 34
            Layout.preferredHeight: 34
            Layout.alignment: Qt.AlignVCenter

            // Two nested outlines read as a processor die without needing an
            // icon set the Quick frontend does not have yet.
            Rectangle {
                width: 15
                height: 15
                color: ExoTheme.surfaceRaised
                border.width: 2
                border.color: root.inspected ? ExoTheme.accent : ExoTheme.textMuted
                radius: 3
                anchors.centerIn: parent
            }
        }

        ColumnLayout {
            spacing: 3
            Layout.fillWidth: true

            RowLayout {
                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                Label {
                    text: root.title
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: ExoTheme.text
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 13
                        weight: Font.DemiBold
                    }
                }

                Label {
                    text: root.kindBadge
                    textFormat: Text.PlainText
                    visible: root.kindBadge !== ""
                    color: ExoTheme.textMuted
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: 10
                    }
                }
            }

            Label {
                text: root.backendLine
                textFormat: Text.PlainText
                elide: Text.ElideRight
                visible: root.backendLine !== ""
                color: ExoTheme.textSecondary
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: 11
                }
            }
        }

        Rectangle {
            visible: root.active
            implicitWidth: activeLabel.implicitWidth + 2 * ExoTheme.spacingSm
            implicitHeight: 20
            color: ExoTheme.surfaceRaised
            border.width: 1
            border.color: ExoTheme.success
            radius: ExoTheme.radiusSm
            Layout.alignment: Qt.AlignVCenter

            Label {
                id: activeLabel

                text: qsTr("ACTIVE")
                textFormat: Text.PlainText
                color: ExoTheme.success
                anchors.centerIn: parent
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 9
                    weight: Font.DemiBold
                }
            }
        }
    }

    background: Rectangle {
        color: root.down ? ExoTheme.surfaceHover : root.hovered ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: 1
        border.color: root.visualFocus ? ExoTheme.text : root.inspected ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusMd
    }
}
