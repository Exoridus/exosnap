import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ExoSnap.Updater

Window {
    id: window
    required property UpdaterViewAdapter adapter
    property bool detailsPreview: false
    Component.onCompleted: if (detailsPreview) details.open()
    title: qsTr("ExoSnap Updater")
    flags: Qt.Window | Qt.FramelessWindowHint
    color: "transparent"
    width: 480
    height: 176
    minimumWidth: 480
    maximumWidth: 480
    minimumHeight: 176
    maximumHeight: 176
    visible: false
    onClosing: function(close) { close.accepted = window.adapter.requestClose() }

    Rectangle {
        anchors.fill: parent
        color: ExoTheme.surface
        border.color: Qt.tint(ExoTheme.surface, ExoTheme.lineStrong)
        radius: 8
        focus: true
        Keys.onReturnPressed: {
            if (!details.visible && !cancelDialog.visible && window.adapter.primaryAction !== "")
                window.adapter.activateAction(window.adapter.primaryAction)
        }
        Keys.onEnterPressed: {
            if (!details.visible && !cancelDialog.visible && window.adapter.primaryAction !== "")
                window.adapter.activateAction(window.adapter.primaryAction)
        }
        Keys.onEscapePressed: {
            if (details.visible) details.close()
            else if (cancelDialog.visible) window.adapter.dismissCancelConfirmation()
            else window.adapter.requestClose()
        }
    }
    ExoSetupHeader {
        objectName: "updaterTitleBar"
        width: parent.width
        height: 40
        role: qsTr("Updater")
        versionText: window.adapter.versionText
        closeEnabled: window.adapter.closeEnabled
        closeAccessibleName: window.adapter.closeAccessibleName
        onMinimizeRequested: window.showMinimized()
        onCloseRequested: window.adapter.requestClose()
        onMoveRequested: window.startSystemMove()
    }
    ColumnLayout {
        x: 24; y: 58
        width: parent.width - 48
        height: 102
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 20
            spacing: 10
            Item {
                id: statusIcon
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                visible: !window.adapter.progressVisible
                readonly property bool circled: window.adapter.panelGlyph === "check" || window.adapter.panelGlyph === "cross"
                readonly property color ink: window.adapter.panelTone === "error" ? ExoTheme.error
                                             : window.adapter.panelTone === "warning" ? ExoTheme.warning
                                             : window.adapter.panelTone === "success" ? ExoTheme.success : ExoTheme.accent
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
                    kind: window.adapter.panelGlyph === "check" ? ExoGlyph.Check
                          : window.adapter.panelGlyph === "cross" ? ExoGlyph.Close
                          : window.adapter.panelGlyph === "warning" ? ExoGlyph.Warning : ExoGlyph.Info
                    color: statusIcon.ink
                }
            }
            Label {
                objectName: "updaterHeadline"
                Layout.fillWidth: true
                text: window.adapter.panelTitle
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.text
                font.family: ExoTheme.sansFamily
                font.pixelSize: 16
                font.weight: Font.DemiBold
                Accessible.name: text
                ToolTip.text: text
                ToolTip.visible: headlineHover.hovered && truncated
                HoverHandler { id: headlineHover }
            }
            BusyIndicator {
                objectName: "updaterBusyIndicator"
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                padding: 0
                visible: window.adapter.progressVisible && window.adapter.indeterminate
                running: visible
                palette.dark: ExoTheme.accent
                Accessible.name: window.adapter.panelTitle
                Accessible.description: window.adapter.ringDescription
            }
            Label {
                visible: window.adapter.progressVisible && !window.adapter.indeterminate
                text: qsTr("%1%").arg(window.adapter.ringPercent)
                color: ExoTheme.textMuted
                font.family: ExoTheme.monoFamily
                font.pixelSize: 13
            }
        }
        ExoSetupProgress {
            objectName: "updaterProgress"
            Layout.fillWidth: true
            Layout.preferredHeight: 4
            Layout.topMargin: 6
            visible: window.adapter.progressVisible
            progress: window.adapter.ring
            indeterminate: window.adapter.indeterminate
            accessibleDescription: window.adapter.ringDescription
        }
        Label {
            objectName: "updaterDetail"
            visible: !window.adapter.progressVisible
            Layout.fillWidth: true
            Layout.leftMargin: 30
            text: window.adapter.compactDetail
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            color: ExoTheme.textMuted
            font.family: ExoTheme.sansFamily
            font.pixelSize: 13
            Accessible.name: text
            ToolTip.text: text
            ToolTip.visible: detailHover.hovered && truncated
            HoverHandler { id: detailHover }
        }
        Item { Layout.fillHeight: true }
        RowLayout {
            objectName: "updaterActionRow"
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            spacing: 6
            ExoButton {
                objectName: "updaterDetailsAction"
                visible: window.adapter.failedTerminal
                text: qsTr("Details")
                quiet: true
                compact: true
                leftPadding: 0
                Layout.preferredHeight: 30
                onClicked: details.open()
            }
            Label {
                objectName: "updaterProgressDetail"
                visible: window.adapter.progressVisible
                Layout.fillWidth: true
                text: window.adapter.compactDetail
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.textMuted
                font.family: window.adapter.panelGlyph === "download" ? ExoTheme.monoFamily : ExoTheme.sansFamily
                font.pixelSize: window.adapter.panelGlyph === "download" ? 12 : 13
                Accessible.name: text
                ToolTip.text: text
                ToolTip.visible: progressDetailHover.hovered && truncated
                HoverHandler { id: progressDetailHover }
            }
            Item { Layout.fillWidth: true; visible: !window.adapter.progressVisible }
            ExoButton {
                objectName: "updaterOpenLogAction"
                visible: window.adapter.failedTerminal && window.adapter.logAvailable
                text: qsTr("Open log")
                quiet: true
                compact: true
                Layout.preferredHeight: 30
                onClicked: window.adapter.openLogRequested()
            }
            ExoButton {
                objectName: "updaterSecondaryAction"
                visible: window.adapter.secondaryAction !== ""
                text: window.adapter.secondaryAction
                compact: true
                Layout.preferredHeight: 30
                onClicked: window.adapter.activateAction(text)
            }
            ExoButton {
                objectName: "updaterCloseAction"
                visible: window.adapter.closeActionVisible && window.adapter.closeActionEnabled
                enabled: window.adapter.closeActionEnabled
                text: window.adapter.closeActionLabel
                compact: true
                quiet: window.adapter.progressVisible || window.adapter.primaryAction !== ""
                Layout.preferredHeight: 30
                onClicked: window.adapter.activateAction(text)
            }
            ExoButton {
                objectName: "updaterPrimaryAction"
                visible: window.adapter.primaryAction !== ""
                text: window.adapter.primaryAction
                tone: "primary"
                compact: true
                Layout.preferredHeight: 30
                onClicked: window.adapter.activateAction(text)
            }
        }
    }
    Popup {
        id: details
        objectName: "updaterDetailsPopup"
        parent: Overlay.overlay
        x: 12; y: 12
        width: window.width - 24
        height: window.height - 24
        modal: true
        focus: true
        padding: 12
        background: Rectangle { color: ExoTheme.surfaceRaised; border.color: ExoTheme.lineStrong; radius: 8 }
        contentItem: ColumnLayout {
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                Label {
                    width: parent.width
                    text: window.adapter.detailsText
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: ExoTheme.textSecondary
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: 13
                    Accessible.name: text
                }
            }
            ExoButton {
                objectName: "updaterDetailsCloseAction"
                Layout.alignment: Qt.AlignRight
                Layout.preferredHeight: 30
                text: qsTr("Close")
                compact: true
                onClicked: details.close()
            }
        }
    }
    Popup {
        id: cancelDialog
        objectName: "updaterCancelDialog"
        parent: Overlay.overlay
        x: 8; y: 8
        width: window.width - 16
        height: window.height - 16
        modal: true
        focus: true
        padding: 12
        visible: window.adapter.cancelConfirmationVisible
        closePolicy: Popup.NoAutoClose
        onOpened: keepUpdatingButton.forceActiveFocus()
        background: Rectangle { color: ExoTheme.surfaceRaised; border.color: ExoTheme.lineStrong; radius: 8 }
        contentItem: ColumnLayout {
            spacing: 6
            Label {
                Layout.fillWidth: true
                text: qsTr("Cancel this update?")
                color: ExoTheme.text
                font.family: ExoTheme.sansFamily
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Download and preparation progress will be discarded. Your installed ExoSnap version remains unchanged.")
                wrapMode: Text.WordWrap
                color: ExoTheme.textSecondary
                font.family: ExoTheme.sansFamily
                font.pixelSize: 13
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ExoButton {
                    objectName: "updaterConfirmCancelButton"
                    text: qsTr("Cancel update")
                    tone: "destructive"
                    compact: true
                    Layout.preferredHeight: 30
                    onClicked: window.adapter.confirmCancelAndClose()
                }
                ExoButton {
                    id: keepUpdatingButton
                    objectName: "updaterKeepUpdatingButton"
                    text: qsTr("Keep updating")
                    tone: "primary"
                    compact: true
                    Layout.preferredHeight: 30
                    onClicked: window.adapter.dismissCancelConfirmation()
                }
            }
        }
    }
}
