import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ExoSnap.Setup

Window {
    id: window
    required property SetupController controller
    property bool screenshotMode: false
    property bool detailsPreview: false
    readonly property int pageHeight: controller.decision ? 300 : 176
    Component.onCompleted: if (detailsPreview) details.open()
    title: qsTr("ExoSnap Setup")
    flags: Qt.Window | Qt.FramelessWindowHint
    color: "transparent"
    width: 480
    height: pageHeight
    minimumWidth: width
    maximumWidth: width
    minimumHeight: pageHeight
    maximumHeight: pageHeight
    visible: false
    onClosing: function(event) { event.accepted = window.controller.requestClose() }

    Rectangle {
        anchors.fill: parent
        color: ExoTheme.surface
        border.color: Qt.tint(ExoTheme.surface, ExoTheme.lineStrong)
        radius: 8
        focus: true
        Keys.onEscapePressed: {
            if (license.visible) license.close()
            else if (details.visible) details.close()
            else window.controller.activateSecondary()
        }
        Keys.onReturnPressed: window.controller.activatePrimary()
        Keys.onEnterPressed: window.controller.activatePrimary()

        ExoSetupHeader {
            id: header
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: implicitHeight
            role: qsTr("Setup")
            versionText: window.controller.versionText
            closeEnabled: window.controller.closeEnabled
            closeAccessibleName: qsTr("Close Setup")
            onMinimizeRequested: window.showMinimized()
            onCloseRequested: window.controller.requestClose()
            onMoveRequested: window.startSystemMove()
        }
        Loader {
            id: body
            anchors.top: header.bottom
            anchors.bottom: footer.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            anchors.topMargin: window.controller.decision ? 22 : 18
            sourceComponent: window.controller.decision ? decisionBody : statusBody
        }
        Component { id: decisionBody; SetupDecision { controller: window.controller } }
        Component { id: statusBody; SetupStatus { controller: window.controller } }

        RowLayout {
            id: footer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 24
            anchors.rightMargin: window.controller.decision ? 16 : 24
            anchors.bottomMargin: window.controller.decision ? 16 : 16
            height: window.controller.decision ? 32 : 30
            spacing: 6
            ExoButton {
                visible: window.controller.page === "install"
                text: qsTr("View license")
                quiet: true
                compact: true
                leftPadding: 0
                onClicked: license.open()
            }
            ExoButton {
                visible: window.controller.page === "failure"
                text: qsTr("Details")
                quiet: true
                compact: true
                leftPadding: 0
                onClicked: details.open()
            }
            Label {
                id: progressDetail
                objectName: "setupProgressDetail"
                visible: window.controller.page === "applying" || window.controller.page === "detecting"
                Layout.fillWidth: true
                text: window.controller.description
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.textMuted
                font.family: ExoTheme.sansFamily
                font.pixelSize: 13
                Accessible.description: text
                ToolTip.text: text
                ToolTip.visible: progressDetailHover.hovered && truncated
                HoverHandler { id: progressDetailHover }
            }
            Item { Layout.fillWidth: true; visible: !progressDetail.visible }
            ExoButton {
                visible: window.controller.page === "failure"
                text: qsTr("Open log")
                quiet: true
                compact: true
                onClicked: window.controller.openLogRequested()
            }
            ExoButton {
                objectName: "setupSecondaryAction"
                visible: text.length > 0
                text: window.controller.secondaryText
                compact: true
                quiet: window.controller.primaryText.length > 0 || progressDetail.visible
                Layout.preferredHeight: footer.height
                onClicked: window.controller.activateSecondary()
            }
            ExoButton {
                id: primary
                objectName: "setupPrimaryAction"
                visible: text.length > 0
                text: window.controller.primaryText
                tone: window.controller.page === "uninstall" && window.controller.removeUserData ? "destructive" : "primary"
                compact: true
                leadingGlyph: window.controller.decision ? ExoGlyph.Shield : ExoGlyph.Invalid
                Layout.preferredHeight: footer.height
                background: Rectangle {
                    radius: 7
                    color: primary.down ? Qt.darker(primary.tone === "destructive" ? ExoTheme.error : ExoTheme.accent, 1.1)
                                        : primary.tone === "destructive" ? ExoTheme.error : ExoTheme.accent
                    border.width: primary.activeFocus ? 2 : 0
                    border.color: ExoTheme.text
                }
                contentItem: RowLayout {
                    spacing: 7
                    ExoGlyph {
                        visible: primary.leadingGlyph !== ExoGlyph.Invalid
                        Layout.preferredWidth: 11
                        Layout.preferredHeight: 11
                        kind: primary.leadingGlyph
                        color: primary.tone === "destructive" ? ExoTheme.errorInk : ExoTheme.accentInk
                    }
                    Label {
                        text: primary.text
                        color: primary.tone === "destructive" ? ExoTheme.errorInk : ExoTheme.accentInk
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                }
                onClicked: window.controller.activatePrimary()
            }
        }
        Popup {
            id: cancellation
            anchors.centerIn: parent
            width: parent.width - 24
            height: 136
            padding: 16
            modal: true
            focus: true
            visible: window.controller.cancelConfirmationVisible
            onClosed: window.controller.dismissCancel()
            background: Rectangle { color: ExoTheme.surfaceRaised; radius: 8; border.color: ExoTheme.lineStrong }
            contentItem: ColumnLayout {
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Cancel Setup?")
                    color: ExoTheme.text
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillHeight: true }
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    ExoButton { compact: true; text: qsTr("Keep going"); onClicked: window.controller.dismissCancel() }
                    ExoButton { compact: true; text: qsTr("Cancel Setup"); onClicked: window.controller.confirmCancel() }
                }
            }
        }
        Popup {
            id: details
            objectName: "setupDetailsPopup"
            anchors.centerIn: parent
            width: parent.width - 24
            height: parent.height - 16
            padding: 12
            modal: true
            focus: true
            background: Rectangle { color: ExoTheme.surfaceRaised; radius: 8; border.color: ExoTheme.lineStrong }
            contentItem: ColumnLayout {
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    TextArea {
                        text: window.controller.detail
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.Wrap
                        readOnly: true
                        selectByMouse: true
                        color: ExoTheme.text
                        font.family: ExoTheme.monoFamily
                        font.pixelSize: 12
                        background: null
                    }
                }
                ExoButton { Layout.alignment: Qt.AlignRight; compact: true; text: qsTr("Close"); onClicked: details.close() }
            }
        }
        Popup {
            id: license
            anchors.centerIn: parent
            width: parent.width - 24
            height: parent.height - 16
            padding: 12
            modal: true
            focus: true
            background: Rectangle { color: ExoTheme.surfaceRaised; radius: 8; border.color: ExoTheme.lineStrong }
            contentItem: ColumnLayout {
                Label { text: qsTr("License"); color: ExoTheme.text; font.family: ExoTheme.sansFamily; font.pixelSize: 16 }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    TextArea {
                        text: window.controller.licenseText
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.Wrap
                        readOnly: true
                        selectByMouse: true
                        color: ExoTheme.text
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: 12
                        background: null
                    }
                }
                ExoButton { Layout.alignment: Qt.AlignRight; compact: true; text: qsTr("Back"); onClicked: license.close() }
            }
        }
    }
}
