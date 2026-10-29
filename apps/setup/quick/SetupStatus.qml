import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property SetupController controller
    readonly property bool applying: controller.page === "applying" || controller.page === "detecting"
    spacing: 0

    RowLayout {
        Layout.fillWidth: true
        spacing: 10
        Item {
            id: statusIcon
            visible: !root.applying
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            readonly property bool circled: root.controller.statusTone === "success" || root.controller.statusTone === "error"
            readonly property color ink: root.controller.statusTone === "success" ? ExoTheme.success
                                         : root.controller.statusTone === "warning" ? ExoTheme.warning
                                         : root.controller.statusTone === "error" ? ExoTheme.error : ExoTheme.textMuted
            Rectangle {
                anchors.centerIn: parent
                width: 18; height: 18
                radius: 9
                color: "transparent"
                border.width: 1.5
                border.color: statusIcon.ink
                visible: statusIcon.circled
                Accessible.ignored: true
            }
            ExoGlyph {
                anchors.centerIn: parent
                width: statusIcon.circled ? 14 : 20
                height: width
                kind: root.controller.statusTone === "success" ? ExoGlyph.Check
                      : root.controller.statusTone === "warning" ? ExoGlyph.Warning
                      : root.controller.statusTone === "error" ? ExoGlyph.Close : ExoGlyph.Info
                color: statusIcon.ink
            }
        }
        Label {
            objectName: "setupHeadline"
            Layout.fillWidth: true
            text: root.controller.headline
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: ExoTheme.text
            font.family: ExoTheme.sansFamily
            font.pixelSize: 16
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
            Accessible.description: text
        }
        BusyIndicator {
            objectName: "setupBusyIndicator"
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
            padding: 0
            visible: root.applying && root.controller.indeterminate
            running: visible
            palette.dark: ExoTheme.accent
            Accessible.name: root.controller.headline
        }
        Label {
            visible: root.applying && !root.controller.indeterminate
            text: qsTr("%1%").arg(Math.round(root.controller.progress * 100))
            color: ExoTheme.textSecondary
            font.family: ExoTheme.monoFamily
            font.pixelSize: 13
        }
    }
    ExoSetupProgress {
        Layout.fillWidth: true
        Layout.topMargin: 12
        visible: root.applying
        progress: root.controller.progress
        indeterminate: root.controller.indeterminate
        accessibleDescription: root.controller.headline
    }
    Label {
        objectName: "setupDescription"
        visible: !root.applying
        Layout.fillWidth: true
        Layout.leftMargin: 30
        Layout.topMargin: 6
        text: root.controller.description
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
        color: ExoTheme.textMuted
        font.family: ExoTheme.sansFamily
        font.pixelSize: 13
        lineHeight: 1.45
        Accessible.description: text
        ToolTip.text: text
        ToolTip.visible: descriptionHover.hovered && truncated
        HoverHandler { id: descriptionHover }
    }
    Item { Layout.fillHeight: true }
}
