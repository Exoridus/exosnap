import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    implicitHeight: 40
    property string role
    property string versionText
    property bool closeEnabled: true
    property string closeAccessibleName: qsTr("Close")
    signal minimizeRequested()
    signal closeRequested()
    signal moveRequested()

    Rectangle { anchors.fill: parent; anchors.margins: 1; radius: 7; color: ExoTheme.surface }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: ExoTheme.line }
    DragHandler {
        target: null
        onActiveChanged: if (active) root.moveRequested()
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 18
        spacing: 10
        Row {
            Label { text: "exo"; color: ExoTheme.text; font.family: ExoTheme.sansFamily; font.pixelSize: 14; font.weight: Font.DemiBold }
            Label { text: "snap"; color: ExoTheme.accent; font.family: ExoTheme.sansFamily; font.pixelSize: 14; font.weight: Font.DemiBold }
        }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 14; color: ExoTheme.lineStrong }
        Label { text: root.role; color: ExoTheme.textSecondary; font.family: ExoTheme.sansFamily; font.pixelSize: 13 }
        Label {
            Layout.fillWidth: true
            text: root.versionText
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: ExoTheme.textMuted
            font.family: ExoTheme.monoFamily
            font.pixelSize: 11
            Accessible.name: text
            ToolTip.text: text
            ToolTip.visible: versionHover.hovered && truncated
            HoverHandler { id: versionHover }
        }
        Row {
            ExoButton {
                objectName: "setupMinimizeButton"
                width: 44; height: 40
                quiet: true
                glyph: ExoGlyph.Minus
                text: qsTr("Minimize")
                onClicked: root.minimizeRequested()
            }
            ExoButton {
                objectName: "setupCloseButton"
                width: 44; height: 40
                quiet: true
                glyph: ExoGlyph.Close
                text: root.closeAccessibleName
                enabled: root.closeEnabled
                onClicked: root.closeRequested()
            }
        }
    }
}
