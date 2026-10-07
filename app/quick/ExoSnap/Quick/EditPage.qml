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
                        objectName: "editJumpStart"
                        text: qsTr("Jump to start")
                        shortcutText: qsTr("Home")
                        glyph: ExoGlyph.JumpStart
                        enabled: root.session.positionMs > 0
                        onClicked: root.session.requestSeek(0)
                    }
                    EditActionButton {
                        objectName: "editSeekBackward"
                        text: qsTr("Back one second")
                        shortcutText: qsTr("Shift+Left")
                        glyph: ExoGlyph.StepBack
                        enabled: root.session.positionMs > 0
                        onClicked: root.session.requestSeek(Math.max(0, root.session.positionMs - 1000))
                    }
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
                    ExoGlyph {
                        kind: ExoGlyph.Speaker
                        color: ExoTheme.textSecondary
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        Accessible.ignored: true
                    }
                    Slider {
                        Layout.preferredWidth: 70
                        Layout.minimumWidth: 50
                        from: 0
                        to: 1
                        value: root.player.volume
                        Accessible.name: qsTr("Preview volume")
                        onMoved: root.player.volume = value
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
            EditActionButton {
                objectName: "editRippleDelete"
                text: qsTr("Ripple delete")
                shortcutText: qsTr("Shift+Delete")
                glyph: ExoGlyph.CloseGap
                enabled: root.session.selectedClip !== 0
                onClicked: root.session.deleteSelected(true)
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
            Slider {
                objectName: "editZoom"
                Layout.minimumWidth: 65
                Layout.preferredWidth: 100
                Layout.maximumWidth: 120
                from: 10
                to: 250
                value: clips.pixelsPerSecond
                Accessible.name: qsTr("Timeline zoom")
                onMoved: clips.zoomTo(value)
            }
            Item { Layout.fillWidth: true }
            Label {
                text: qsTr("%1%").arg(root.exporter.progressPercent)
                visible: root.exporter.running
                color: ExoTheme.textSecondary
                Accessible.name: qsTr("Exporting %1%").arg(root.exporter.progressPercent)
            }
            ExoSelect {
                options: root.exporter.profileOptions
                value: root.exporter.profileKey
                Layout.preferredWidth: 160
                Accessible.name: qsTr("Export preset")
                onValueActivated: value => root.exporter.profileKey = value
            }
            EditActionButton {
                objectName: "editExport"
                text: qsTr("Export")
                glyph: ExoGlyph.Send
                tone: "primary"
                enabled: root.session.durationMs > 0 && !root.exporter.running
                onClicked: root.exporter.chooseDestination()
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
