import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    required property SetupController controller

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Label {
            Layout.fillWidth: true
            text: root.controller.headline
            color: ExoTheme.text
            font.family: ExoTheme.sansFamily
            font.pixelSize: 20
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }
        Label {
            Layout.fillWidth: true
            Layout.topMargin: 6
            visible: text.length > 0
            text: root.controller.description
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.textMuted
            font.family: ExoTheme.sansFamily
            font.pixelSize: 13
            lineHeight: 1.5
        }
        Rectangle {
            visible: root.controller.page === "install"
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            Layout.topMargin: 18
            radius: 8
            color: ExoTheme.background
            border.color: ExoTheme.line
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10
                ExoGlyph { Layout.preferredWidth: 14; Layout.preferredHeight: 14; kind: ExoGlyph.Folder; color: ExoTheme.textMuted }
                Label {
                    Layout.fillWidth: true
                    text: root.controller.installPath
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: ExoTheme.textSecondary
                    font.family: ExoTheme.monoFamily
                    font.pixelSize: 12
                    Accessible.description: text
                    ToolTip.text: text
                    ToolTip.visible: pathHover.hovered && truncated
                    HoverHandler { id: pathHover }
                }
            }
        }
        ExoCheckBox {
            id: shortcut
            objectName: "setupDesktopShortcut"
            visible: root.controller.page === "install"
            Layout.topMargin: 10
            text: qsTr("Create a desktop shortcut")
            checked: root.controller.desktopShortcut
            onToggledByUser: function(value) { root.controller.desktopShortcut = value }
            font.family: ExoTheme.sansFamily
            font.pixelSize: 14
            palette.windowText: ExoTheme.text
            palette.text: ExoTheme.text
            palette.highlight: ExoTheme.accent
            palette.base: ExoTheme.background
        }
        ColumnLayout {
            visible: root.controller.page === "uninstall"
            Layout.fillWidth: true
            Layout.topMargin: 16
            spacing: 3
            ExoCheckBox {
                id: deleteData
                objectName: "setupRemoveUserData"
                text: qsTr("Also delete my ExoSnap data")
                Accessible.description: deleteDescription.text
                checked: root.controller.removeUserData
                onToggledByUser: function(value) { root.controller.removeUserData = value }
                palette.highlight: ExoTheme.error
                palette.base: ExoTheme.background
                indicator: Rectangle {
                    implicitWidth: 18
                    implicitHeight: 18
                    y: deleteData.topPadding + (deleteData.availableHeight - height) / 2
                    radius: 3
                    color: deleteData.checked ? ExoTheme.error : ExoTheme.surfaceHover
                    border.color: deleteData.checked ? ExoTheme.error : ExoTheme.lineStrong
                    ExoGlyph {
                        anchors.centerIn: parent
                        width: 12; height: 12
                        kind: ExoGlyph.Check
                        color: ExoTheme.errorInk
                        visible: deleteData.checked
                    }
                }
            }
            Label {
                    id: deleteDescription
                    Layout.fillWidth: true
                    Layout.leftMargin: 26
                    text: qsTr("Settings, presets, history, caches, recovery data, logs and crash reports. This can't be undone.")
                    wrapMode: Text.WordWrap
                    color: ExoTheme.textMuted
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: 12
                    lineHeight: 1.45
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 110
            Layout.topMargin: 16
            visible: root.controller.page === "manage"
            color: "transparent"
            border.color: ExoTheme.line
            radius: 10
            Column {
                anchors.fill: parent
                Repeater {
                    model: [
                        { title: qsTr("Repair"), detail: qsTr("Replace missing or damaged program files"), glyph: ExoGlyph.Refresh },
                        { title: qsTr("Uninstall"), detail: qsTr("Remove ExoSnap from this computer"), glyph: ExoGlyph.Trash }
                    ]
                    delegate: AbstractButton {
                        id: manageAction
                        required property var modelData
                        required property int index
                        width: parent.width
                        height: 55
                        activeFocusOnTab: true
                        hoverEnabled: true
                        Accessible.name: modelData.title
                        Accessible.description: modelData.detail
                        onClicked: index === 0 ? root.controller.chooseRepair() : root.controller.chooseUninstall()
                        background: Rectangle {
                            radius: 10
                            color: manageAction.hovered || manageAction.activeFocus ? ExoTheme.surfaceHover : "transparent"
                            Rectangle { visible: manageAction.index > 0; width: parent.width; height: 1; color: ExoTheme.line }
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            ExoGlyph { Layout.preferredWidth: 16; Layout.preferredHeight: 16; kind: manageAction.modelData.glyph; color: ExoTheme.textSecondary }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label { text: manageAction.modelData.title; color: ExoTheme.text; font.family: ExoTheme.sansFamily; font.pixelSize: 14; font.weight: Font.DemiBold }
                                Label { text: manageAction.modelData.detail; color: ExoTheme.textMuted; font.family: ExoTheme.sansFamily; font.pixelSize: 12 }
                            }
                            ExoGlyph { Layout.preferredWidth: 12; Layout.preferredHeight: 12; kind: ExoGlyph.ArrowRight; color: ExoTheme.textMuted }
                        }
                    }
                }
            }
        }
        Item { Layout.fillHeight: true }
    }
}
