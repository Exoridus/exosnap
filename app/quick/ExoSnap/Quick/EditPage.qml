pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: root
    required property EditSessionAdapter session
    required property EditTimelineAdapter timeline
    required property EditPlayerAdapter player
    required property EditExportAdapter exporter
    required property RecordViewModelAdapter recordings
    objectName: "quickEditPage"
    Keys.forwardTo: [clips]
    Binding { target: root.timeline; property: "trackWidth"; value: Math.min(1000, root.width) }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: ExoTheme.spacingMd
        spacing: ExoTheme.spacingSm

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: ExoTheme.spacingMd
            EditSourceBrowser {
                session: root.session
                recordings: root.recordings
                timeline: root.timeline
                Layout.preferredWidth: Math.max(220, root.width * 0.28)
                Layout.maximumWidth: 380
                Layout.fillHeight: true
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                EditPlayer {
                    session: root.session
                    player: root.player
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 100
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    EditActionButton {
                        objectName: "editTransportPlay"
                        text: root.player.playing ? qsTr("Pause") : qsTr("Play")
                        shortcutText: qsTr("Space")
                        glyph: root.player.playing ? ExoGlyph.Pause : ExoGlyph.Run
                        enabled: root.session.durationMs > 0 && root.player.clipOpen
                        onClicked: root.player.togglePlay()
                    }
                    Label {
                        text: root.session.formatTimestamp(root.session.positionMs) + " / " + root.session.formatTimestamp(root.session.durationMs)
                        color: ExoTheme.textSecondary
                        font.family: ExoTheme.monoFamily
                        font.pixelSize: ExoTheme.fontCaption
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Playback position")
                    }
                    EditActionButton {
                        id: volumeButton
                        objectName: "editVolume"
                        text: root.player.volume === 0 ? qsTr("Preview volume (muted)") : qsTr("Preview volume")
                        glyph: ExoGlyph.Speaker
                        checked: root.player.volume === 0
                        onClicked: volumePopup.open()
                        Popup {
                            id: volumePopup
                            objectName: "editVolumePopup"
                            y: -height - 4
                            x: volumeButton.width - width
                            width: 160
                            padding: 12
                            focus: true
                            onOpened: volumeSlider.forceActiveFocus(Qt.PopupFocusReason)
                            onClosed: volumeButton.forceActiveFocus(Qt.PopupFocusReason)
                            Slider {
                                id: volumeSlider
                                objectName: "editVolumeSlider"
                                width: parent.width
                                from: 0
                                to: 1
                                value: root.player.volume
                                Accessible.name: qsTr("Preview volume")
                                onMoved: root.player.volume = value
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            EditActionButton {
                objectName: "editUndo"
                text: qsTr("Undo")
                shortcutText: qsTr("Ctrl+Z")
                glyph: ExoGlyph.Undo
                enabled: root.session.canUndo
                onClicked: root.session.undo()
            }
            EditActionButton {
                objectName: "editRedo"
                text: qsTr("Redo")
                shortcutText: qsTr("Ctrl+Y")
                glyph: ExoGlyph.Redo
                enabled: root.session.canRedo
                onClicked: root.session.redo()
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: ExoTheme.line }
            EditActionButton {
                objectName: "editSplit"
                text: qsTr("Split")
                shortcutText: qsTr("Ctrl+B")
                glyph: ExoGlyph.Scissors
                enabled: root.session.durationMs > 0
                onClicked: root.session.splitSelected()
            }
            EditActionButton {
                objectName: "editDelete"
                text: qsTr("Delete")
                shortcutText: qsTr("Delete")
                glyph: ExoGlyph.Trash
                enabled: root.session.selectedClip !== 0
                onClicked: root.session.deleteSelected(false)
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: ExoTheme.line }
            EditActionButton {
                objectName: "editSnapping"
                text: qsTr("Snapping")
                glyph: ExoGlyph.Magnet
                checkable: true
                checked: clips.snapping
                onClicked: clips.snapping = checked
            }
            Item { Layout.fillWidth: true }
            Label {
                text: qsTr("%1%").arg(root.exporter.progressPercent)
                visible: root.exporter.running
                color: ExoTheme.textSecondary
                Accessible.name: qsTr("Exporting %1%").arg(root.exporter.progressPercent)
            }
            Row {
                spacing: 0
                EditActionButton {
                    id: exportAction
                    objectName: "editExport"
                    text: qsTr("Export — %1").arg(root.exporter.profileOptions.find(option => option.value === root.exporter.profileKey)?.label || "")
                    glyph: ExoGlyph.Send
                    tone: "primary"
                    enabled: root.session.durationMs > 0 && !root.exporter.running
                    onClicked: root.exporter.chooseDestination()
                    Binding { target: exportAction.background; property: "topRightRadius"; value: 0 }
                    Binding { target: exportAction.background; property: "bottomRightRadius"; value: 0 }
                }
                EditActionButton {
                    id: exportProfiles
                    objectName: "editExportProfiles"
                    text: qsTr("Export profile")
                    glyph: ExoGlyph.Send
                    tone: "primary"
                    implicitWidth: 26
                    contentItem: Item {
                        ExoChevron { anchors.centerIn: parent; tone: exportProfiles._ink }
                    }
                    Binding { target: exportProfiles.background; property: "topLeftRadius"; value: 0 }
                    Binding { target: exportProfiles.background; property: "bottomLeftRadius"; value: 0 }
                    onClicked: profileMenu.open()
                    Menu {
                        id: profileMenu
                        objectName: "editExportProfileMenu"
                        x: exportProfiles.width - width
                        y: exportProfiles.height
                        Repeater {
                            model: root.exporter.profileOptions.filter(option => option.selectable)
                            MenuItem {
                                required property var modelData
                                text: modelData.label
                                checkable: true
                                checked: root.exporter.profileKey === modelData.value
                                onTriggered: root.exporter.profileKey = modelData.value
                            }
                        }
                    }
                }
            }
        }
        Label {
            text: root.session.workspaceError || root.exporter.errorText
            visible: text.length > 0
            color: ExoTheme.textSecondary
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        EditTimeline {
            id: clips
            session: root.session
            player: root.player
            thumbnails: root.timeline
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(180, root.height * 0.37)
        }
    }
}
