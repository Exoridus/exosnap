import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Search pill: a leading magnifier glyph and a frameless input. The two are laid
// out explicitly rather than through a leading action, so the glyph stays centred
// at any field height.
Rectangle {
    id: root

    property alias text: input.text
    property alias placeholderText: input.placeholderText

    signal searchEdited(string query)

    implicitHeight: 30
    implicitWidth: 220
    color: ExoTheme.surface
    border.width: 1
    border.color: input.activeFocus ? ExoTheme.accent : ExoTheme.line
    radius: ExoTheme.radiusSm

    RowLayout {
        spacing: ExoTheme.spacingSm
        anchors {
            fill: parent
            leftMargin: ExoTheme.spacingMd
            rightMargin: ExoTheme.spacingSm
        }

        ExoGlyph {
            kind: ExoGlyph.Search
            color: ExoTheme.textMuted
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 15
            Layout.preferredHeight: 15
        }

        TextField {
            id: input

            selectByMouse: true
            color: ExoTheme.text
            placeholderTextColor: ExoTheme.textDim
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            Accessible.name: root.placeholderText
            background: null
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
            onTextEdited: root.searchEdited(input.text)
        }

        AbstractButton {
            id: clearButton

            implicitWidth: 16
            implicitHeight: 16
            visible: input.text !== ""
            hoverEnabled: true
            Layout.alignment: Qt.AlignVCenter
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Clear search")
            onClicked: {
                input.text = "";
                root.searchEdited("");
            }

            contentItem: ExoGlyph {
                kind: ExoGlyph.Close
                color: clearButton.hovered ? ExoTheme.text : ExoTheme.textMuted
                width: 12
                height: 12
            }
        }
    }
}
