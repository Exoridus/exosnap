import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The destination folder as one control: the path is the button that changes it.
//
// It replaces a text field and a Browse button beside it. Two controls for one
// value meant the field carried the whole slot's width and still elided a real
// folder away, while the only thing anyone did with it was press the button next
// to it. A path is chosen, not typed.
AbstractButton {
    id: root

    required property string path

    // The tail is the part that identifies a folder; the head is almost always
    // the same user profile. Eliding right hid exactly what the user came to
    // read.
    text: root.path
    hoverEnabled: root.enabled
    focusPolicy: Qt.StrongFocus
    implicitHeight: ExoTheme.controlHeight
    implicitWidth: pathRow.implicitWidth + leftPadding + rightPadding
    leftPadding: ExoTheme.spacingMd
    rightPadding: ExoTheme.spacingMd

    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Destination folder")
    Accessible.description: root.path

    ToolTip.visible: root.hovered && pathLabel.truncated
    ToolTip.delay: 400
    ToolTip.text: root.path

    HoverHandler {
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
    }

    background: Rectangle {
        opacity: root.enabled ? 1.0 : ExoTheme.disabledOpacity
        color: root.down ? ExoTheme.surfaceHover
             : root.hovered ? ExoTheme.hoverTint(ExoTheme.surfaceRaised)
             : ExoTheme.surfaceRaised
        border.width: 1
        border.color: root.visualFocus ? ExoTheme.text : ExoTheme.lineStrong
        radius: ExoTheme.radiusSm
    }

    contentItem: RowLayout {
        id: pathRow

        spacing: ExoTheme.spacingSm

        ExoGlyph {
            kind: ExoGlyph.Folder
            color: root.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
            Layout.preferredWidth: 14
            Layout.preferredHeight: 14
            Layout.alignment: Qt.AlignVCenter
        }

        Label {
            id: pathLabel

            text: root.path
            textFormat: Text.PlainText
            elide: Text.ElideLeft
            verticalAlignment: Text.AlignVCenter
            color: root.enabled ? ExoTheme.text : ExoTheme.textDim
            Layout.fillWidth: true
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        // The chip is the same shape as the text field a row below it, so
        // without a mark it invites typing. The ellipsis is what this platform
        // has always put on the control that opens a picker, and it is what the
        // Browse button beside this row used to be named.
        Label {
            text: "…"
            textFormat: Text.PlainText
            color: root.enabled ? ExoTheme.textMuted : ExoTheme.textDim
            Layout.alignment: Qt.AlignVCenter
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontBody
            }
        }
    }
}
