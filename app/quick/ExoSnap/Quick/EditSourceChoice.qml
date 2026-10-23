pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    required property EditSessionAdapter session
    readonly property var draft: session.sourceAudioChoice
    objectName: "editSourceAudioChoice"
    title: draft.active ? qsTr("Audio: %1").arg(draft.name || "") : qsTr("Choose source audio")
    modal: true
    focus: true
    padding: ExoTheme.spacingLg
    palette.window: ExoTheme.surfaceRaised
    palette.windowText: ExoTheme.text
    palette.text: ExoTheme.text
    palette.buttonText: ExoTheme.text
    Overlay.modal: Rectangle { color: ExoTheme.overlayScrim }
    background: Rectangle {
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusLg
    }
    header: Label {
        text: root.title
        textFormat: Text.PlainText
        color: ExoTheme.text
        leftPadding: ExoTheme.spacingLg
        rightPadding: ExoTheme.spacingLg
        topPadding: ExoTheme.spacingLg
        font.family: ExoTheme.sansFamily
        font.pixelSize: ExoTheme.fontSectionTitle
        font.weight: Font.DemiBold
    }
    width: Math.min(parent.width - 32, 520)
    height: Math.min(parent.height - 32, implicitHeight)
    anchors.centerIn: parent
    onClosed: session.cancelSourceAudioChoice()
    Component.onDestruction: if (session) session.cancelSourceAudioChoice()
    Connections {
        target: root.session
        function onSourceAudioChoiceChanged(): void {
            if (root.opened && !root.draft.active)
                root.close();
        }
    }
    contentItem: ExoScrollView {
        id: choiceScroll
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        clip: true
        implicitHeight: content.implicitHeight
        ColumnLayout {
            id: content
            width: choiceScroll.availableWidth
            spacing: ExoTheme.spacingMd
            ListView {
                id: sources
                Layout.fillWidth: true
                Layout.preferredHeight: count > 0 ? 240 : 0
                clip: true
                reuseItems: true
                visible: !root.draft.active
                model: root.visible && !root.draft.active ? root.session.sourceAudioChoices : []
                delegate: ExoButton {
                    required property var modelData
                    width: sources.width
                    text: modelData.name
                    Accessible.name: text
                    enabled: modelData.available
                    Accessible.description: modelData.reason || ""
                    onClicked: root.session.beginSourceAudioChoice(modelData.assetId)
                }
            }
            Repeater {
                model: root.visible && root.draft.active ? (root.draft.tracks || []).length : 0
                ExoCheckBox {
                    required property int index
                    readonly property var modelData: (root.draft.tracks || [])[index] || ({})
                    objectName: "editSourceAudioTrack-" + modelData.id
                    Layout.fillWidth: true
                    text: qsTr("%1 · %2 · %3 channels").arg(modelData.name || "").arg(modelData.role || qsTr("Audio")).arg(modelData.channels ?? 0)
                    Accessible.name: text
                    checked: modelData.selected ?? false
                    enabled: (modelData.enabled ?? false) && !root.draft.busy
                    Accessible.description: modelData.reason || ""
                    onToggledByUser: selected => root.session.setSourceAudioTrack(modelData.id, selected)
                }
            }
            ExoCheckBox {
                objectName: "editSourceAudioOverlap"
                Layout.fillWidth: true
                visible: root.draft.active && root.draft.overlapRequired
                text: qsTr("Include the named streams together despite overlapping content")
                Accessible.name: text
                checked: root.draft.overlapConfirmed ?? false
                enabled: !root.draft.busy
                onToggledByUser: selected => root.session.setSourceAudioOverlapConfirmed(selected)
            }
            Label {
                Layout.fillWidth: true
                text: root.draft.reason || ""
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: ExoTheme.textSecondary
            }
            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: root.draft.busy ?? false
                visible: running
                Accessible.name: qsTr("Checking source audio")
            }
        }
    }
    footer: DialogButtonBox {
        ExoButton {
            text: qsTr("Cancel")
            Accessible.name: text
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.close()
        }
        ExoButton {
            objectName: "editSourceAudioApply"
            text: qsTr("Apply")
            Accessible.name: text
            tone: "primary"
            enabled: root.draft.canApply ?? false
            visible: root.draft.active ?? false
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.session.applySourceAudioChoice()
        }
    }
}
