pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

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
                    ExoButton {
                        text: qsTr("Previous")
                        compact: true
                        onClicked: root.session.requestSeek(Math.max(0, root.session.positionMs - 1000))
                    }
                    ExoButton {
                        text: root.player.playing ? qsTr("Pause") : qsTr("Play")
                        compact: true
                        enabled: root.session.durationMs > 0
                        onClicked: root.player.togglePlay()
                    }
                    Label {
                        text: root.session.formatTimestamp(root.session.positionMs) + " / " + root.session.formatTimestamp(root.session.durationMs)
                        color: ExoTheme.textSecondary
                        font.family: ExoTheme.monoFamily
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Playback position")
                    }
                    Slider {
                        Layout.preferredWidth: 90
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
            ExoButton {
                leftPadding: 6; rightPadding: 6; text: "↶"; Accessible.name: qsTr("Undo"); ToolTip.text: qsTr("Undo"); ToolTip.visible: hovered; compact: true; enabled: root.session.canUndo; onClicked: root.session.undo() }
            ExoButton {
                leftPadding: 6; rightPadding: 6; text: "↷"; Accessible.name: qsTr("Redo"); ToolTip.text: qsTr("Redo"); ToolTip.visible: hovered; compact: true; enabled: root.session.canRedo; onClicked: root.session.redo() }
            ExoButton {
                leftPadding: 6; rightPadding: 6; text: qsTr("Split"); compact: true; onClicked: root.session.splitSelected() }
            ExoButton {
                leftPadding: 6; rightPadding: 6; text: qsTr("Delete"); compact: true; onClicked: root.session.deleteSelected(false) }
            ExoButton {
                leftPadding: 6; rightPadding: 6; text: qsTr("Ripple"); Accessible.name: qsTr("Ripple delete"); compact: true; onClicked: root.session.deleteSelected(true) }
            ExoButton {
                leftPadding: 6; rightPadding: 6; text: qsTr("Snap"); compact: true; checkable: true; checked: true; onToggled: clips.snapping = checked }
            Slider {
                Layout.minimumWidth: 55
                Layout.preferredWidth: 100
                Layout.fillWidth: true
                from: 10
                to: 250
                value: 40
                Accessible.name: qsTr("Timeline zoom")
                onMoved: clips.pixelsPerSecond = value
            }
            ExoSelect {
                options: root.exporter.profileOptions
                value: root.exporter.profileKey
                Layout.preferredWidth: 150
                Accessible.name: qsTr("Export preset")
                onValueActivated: value => root.exporter.profileKey = value
            }
            ExoButton {
                leftPadding: 6; rightPadding: 6
                text: root.exporter.running ? qsTr("Exporting %1%").arg(root.exporter.progressPercent) : qsTr("Export")
                compact: true
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
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(180, root.height * 0.37)
        }
    }
}
