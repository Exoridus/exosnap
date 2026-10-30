pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property EditSessionAdapter session
    required property EditPlayerAdapter player
    property RecordingReviewAdapter audioController: null
    property bool controlsIdle: false
    readonly property bool controlsHeld: closeButton.visualFocus || playPause.visualFocus || seek.visualFocus || audio.visualFocus || seek.pressed || root.player.scrubbing || audioPanel.visible
    readonly property bool controlsVisible: !root.player.playing || !controlsIdle || controlsHeld
    signal closeRequested
    objectName: "recordingReview"
    color: ExoTheme.overlaySurface
    radius: ExoTheme.radiusLg
    clip: true
    onControlsHeldChanged: revealControls()
    onVisibleChanged: revealControls()

    function revealControls(): void {
        controlsIdle = false;
        if (hideControls.running)
            hideControls.restart();
    }

    Connections {
        target: root.player
        function onPlayingChanged(): void {
            if (!root.player.playing)
                root.revealControls();
        }
    }
    Timer {
        id: hideControls
        interval: 2500
        running: root.visible && root.player.playing && !root.controlsHeld && !root.controlsIdle
        onTriggered: root.controlsIdle = true
    }
    HoverHandler {
        property point lastPosition: Qt.point(-1, -1)
        onPointChanged: {
            // Hover can update without motion when the controls change opacity.
            if (point.position.x !== lastPosition.x || point.position.y !== lastPosition.y) {
                lastPosition = point.position;
                root.revealControls();
            }
        }
    }

    component OverlayButton: ExoButton {
        id: button
        compact: true
        quiet: true
        contentItem: Item {
            ExoGlyph {
                anchors.centerIn: parent
                width: 18
                height: 18
                kind: button.glyph
                color: button.enabled && !button.checked ? ExoTheme.overlayInk : ExoTheme.overlayInkMuted
            }
        }
        background: Rectangle {
            radius: ExoTheme.radiusSm
            color: button.hovered || button.down ? ExoTheme.overlayLine : "transparent"
            border.width: button.visualFocus ? 1 : 0
            border.color: ExoTheme.accent
        }
    }

    function showAudio(): void {
        closeAudio.stop();
        audioPanel.open();
    }

    ExoEditPlayerItem {
        id: surface
        objectName: "reviewSurface"
        anchors.fill: parent
        visible: root.player.harnessStillSource.toString().length === 0
        playerAdapter: visible ? root.player : null
        cornerRadius: root.radius
    }
    Image {
        id: still
        objectName: "reviewHarnessStill"
        anchors.fill: parent
        source: root.player.harnessStillSource
        sourceSize: Qt.size(width, height)
        fillMode: Image.PreserveAspectFit
        visible: source.toString().length > 0
        Accessible.ignored: true
    }
    MouseArea {
        anchors.fill: parent
        enabled: root.player.clipOpen && root.player.previewState !== EditPlayerAdapter.Failed
        onClicked: root.player.togglePlay()
    }
    Label {
        anchors.centerIn: parent
        width: parent.width - 48
        visible: !surface.hasFrame && !still.visible
        text: root.player.placeholderText.length > 0 ? root.player.placeholderText : qsTr("Opening recording")
        textFormat: Text.PlainText
        font.family: ExoTheme.sansFamily
        font.pixelSize: ExoTheme.fontBody
        color: ExoTheme.overlayInkSecondary
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }
    Label {
        readonly property string editError: root.audioController ? root.audioController.editError : ""
        objectName: "reviewMessage"
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: ExoTheme.spacingSm
        width: parent.width - 96
        padding: ExoTheme.spacingSm
        visible: editError.length > 0 || ((surface.hasFrame || still.visible) && root.player.placeholderText.length > 0)
        text: editError.length > 0 ? editError : root.player.placeholderText
        textFormat: Text.PlainText
        font.family: ExoTheme.sansFamily
        font.pixelSize: ExoTheme.fontBody
        color: ExoTheme.overlayInk
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        Accessible.role: Accessible.AlertMessage
        background: Rectangle {
            color: ExoTheme.overlaySurface
            radius: ExoTheme.radiusSm
        }
    }
    OverlayButton {
        id: closeButton
        objectName: "reviewClose"
        opacity: root.controlsVisible ? 1 : 0
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: ExoTheme.spacingSm
        compact: true
        quiet: true
        glyph: ExoGlyph.Close
        text: qsTr("Close review")
        ToolTip.visible: hovered
        ToolTip.text: text
        onClicked: root.closeRequested()
    }

    Rectangle {
        id: controls
        objectName: "reviewControls"
        // Keep hidden controls in the tab chain so keyboard focus can reveal them.
        opacity: root.controlsVisible ? 1 : 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 64
        bottomLeftRadius: root.radius
        bottomRightRadius: root.radius
        gradient: Gradient {
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 1; color: ExoTheme.overlaySurface }
        }
        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 48
            anchors.leftMargin: ExoTheme.spacingLg
            anchors.rightMargin: ExoTheme.spacingLg
            spacing: ExoTheme.spacingMd
            OverlayButton {
                id: playPause
                objectName: "reviewPlayPause"
                compact: true
                quiet: true
                glyph: root.player.playing ? ExoGlyph.Pause : ExoGlyph.Run
                text: root.player.playing ? qsTr("Pause") : qsTr("Play")
                enabled: root.player.clipOpen && root.player.previewState !== EditPlayerAdapter.Failed
                ToolTip.visible: hovered
                ToolTip.text: text
                onClicked: root.player.togglePlay()
            }
            ExoSlider {
                id: seek
                objectName: "reviewSeek"
                Layout.fillWidth: true
                Layout.minimumWidth: 32
                from: 0
                to: Math.max(1, root.session.durationMs)
                enabled: root.player.clipOpen && root.session.durationMs > 0
                Accessible.name: qsTr("Playback position")
                value: root.session.positionMs
                background: Rectangle {
                    x: seek.leftPadding
                    y: seek.topPadding + (seek.availableHeight - height) / 2
                    width: seek.availableWidth
                    height: 3
                    radius: height / 2
                    color: ExoTheme.overlayLine
                    Rectangle {
                        width: seek.visualPosition * parent.width
                        height: parent.height
                        radius: parent.radius
                        color: ExoTheme.overlayInk
                    }
                }
                handle: Rectangle {
                    x: seek.leftPadding + seek.visualPosition * (seek.availableWidth - width)
                    y: seek.topPadding + (seek.availableHeight - height) / 2
                    implicitWidth: 12
                    implicitHeight: 12
                    radius: 6
                    color: seek.enabled ? ExoTheme.overlayInk : ExoTheme.overlayInkMuted
                    border.width: 2
                    border.color: seek.visualFocus || seek.pressed ? ExoTheme.accent : ExoTheme.overlayLine
                }
                onPressedChanged: {
                    if (pressed)
                        root.player.beginScrub();
                    else {
                        root.session.finishPointerSeek();
                        root.player.endScrub();
                    }
                }
                onMovedByUser: value => root.session.requestSeek(Math.round(value))
            }
            Label {
                text: root.session.formatClock(root.session.positionMs) + " / " + root.session.formatClock(root.session.durationMs)
                color: ExoTheme.overlayInk
                font.family: ExoTheme.monoFamily
                font.pixelSize: ExoTheme.fontCaption
            }
            OverlayButton {
                id: audio
                property bool held: false
                objectName: "reviewAudio"
                compact: true
                quiet: true
                glyph: ExoGlyph.Speaker
                text: root.player.monitoringTracks.length === 0 ? qsTr("No playable audio tracks") : root.audioController && root.audioController.muted ? qsTr("Unmute playback") : qsTr("Mute playback")
                checked: root.audioController ? root.audioController.muted : false
                Accessible.description: qsTr("Focus or hover for playback volume and audio tracks")
                ToolTip.visible: hovered && !audioPanel.opened
                ToolTip.text: text
                onClicked: {
                    if (!held && root.audioController && root.player.monitoringTracks.length > 0)
                        root.audioController.muted = !root.audioController.muted;
                    else if (!held)
                        root.showAudio();
                    held = false;
                }
                onPressedChanged: if (pressed)
                    held = false
                onActiveFocusChanged: if (activeFocus)
                    root.showAudio()
                onHoveredChanged: {
                    if (hovered)
                        openAudio.restart();
                    else {
                        openAudio.stop();
                        closeAudio.restart();
                    }
                }
                onPressAndHold: {
                    held = true;
                    root.showAudio();
                }
                Keys.onDownPressed: {
                    root.showAudio();
                    audioPanel.forceActiveFocus();
                }
                Keys.onEscapePressed: event => {
                    audioPanel.close();
                    event.accepted = true;
                }
            }
        }
    }
    Timer {
        id: openAudio
        interval: 150
        onTriggered: root.showAudio()
    }
    Timer {
        id: closeAudio
        interval: 225
        onTriggered: {
            if (!audio.hovered && !audio.activeFocus && !panelHover.hovered && !audioPanel.activeFocus)
                audioPanel.close();
        }
    }
    Popup {
        id: audioPanel
        objectName: "reviewAudioPanel"
        parent: root
        x: Math.max(8, root.width - width - 8)
        y: Math.max(8, controls.y - height - 4)
        width: Math.min(320, root.width - 16)
        height: Math.min(implicitHeight, Math.max(80, controls.y - 12))
        padding: ExoTheme.spacingMd
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onActiveFocusChanged: if (!activeFocus)
            closeAudio.restart()
        background: Rectangle {
            color: ExoTheme.surfaceRaised
            radius: ExoTheme.radiusMd
            HoverHandler {
                id: panelHover
                parent: audioPanel.contentItem.parent
                onHoveredChanged: {
                    if (hovered)
                        closeAudio.stop();
                    else
                        closeAudio.restart();
                }
            }
        }
        contentItem: Flickable {
            Keys.onEscapePressed: event => {
                audioPanel.close();
                event.accepted = true;
            }
            implicitHeight: panelContent.implicitHeight
            contentHeight: panelContent.implicitHeight
            clip: true
            ColumnLayout {
                id: panelContent
                width: parent.width
                spacing: ExoTheme.spacingSm
                Label {
                    text: qsTr("Playback audio")
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontBody
                    color: ExoTheme.text
                    font.weight: Font.DemiBold
                }
                Label {
                    visible: root.player.monitoringTracks.length === 0
                    text: qsTr("No playable audio tracks")
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontCaption
                    color: ExoTheme.textSecondary
                }
                Repeater {
                    model: root.player.monitoringTracks
                    delegate: ExoButton {
                        id: trackButton
                        required property var modelData
                        objectName: "reviewTrack-" + modelData.id
                        Layout.fillWidth: true
                        implicitHeight: trackText.implicitHeight + trackButton.topPadding + trackButton.bottomPadding
                        topPadding: 8
                        bottomPadding: 8
                        leftPadding: 12
                        rightPadding: 12
                        quiet: true
                        selected: modelData.selected
                        enabled: modelData.available && root.audioController !== null && !root.player.transportPending
                        text: modelData.name
                        Accessible.description: modelData.sources || qsTr("Sources unknown")
                        Accessible.role: Accessible.RadioButton
                        Accessible.checkable: true
                        Accessible.checked: modelData.selected
                        background: Rectangle {
                            radius: ExoTheme.radiusSm
                            readonly property color restingColor: trackButton.selected ? ExoTheme.accentTint(ExoTheme.surfaceRaised, 0.12) : ExoTheme.surfaceRaised
                            color: !trackButton.enabled ? restingColor : trackButton.down ? ExoTheme.pressTint(restingColor) : trackButton.hovered ? ExoTheme.hoverTint(restingColor) : restingColor
                            border.width: trackButton.visualFocus ? 1 : 0
                            border.color: ExoTheme.accent
                        }
                        contentItem: Column {
                            id: trackText
                            spacing: 4
                            Label {
                                width: parent.width
                                text: trackButton.modelData.name
                                font.family: ExoTheme.sansFamily
                                font.pixelSize: ExoTheme.fontBody
                                font.weight: trackButton.checked ? Font.DemiBold : Font.Normal
                                textFormat: Text.PlainText
                                color: trackButton.enabled ? ExoTheme.text : ExoTheme.textDim
                                elide: Text.ElideRight
                            }
                            Label {
                                width: parent.width
                                text: trackButton.modelData.sources || qsTr("Sources unknown")
                                font.family: ExoTheme.sansFamily
                                textFormat: Text.PlainText
                                color: trackButton.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
                                font.pixelSize: ExoTheme.fontCaption
                                elide: Text.ElideRight
                            }
                        }
                        onClicked: root.audioController.selectTrack(modelData.id)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    ExoSlider {
                        objectName: "reviewVolume"
                        Layout.fillWidth: true
                        from: 0
                        to: 100
                        stepSize: 1
                        enabled: root.audioController !== null && root.player.monitoringTracks.length > 0
                        value: root.audioController ? root.audioController.volume * 100 : 100
                        Accessible.name: qsTr("Playback volume")
                        onMovedByUser: value => root.audioController.volume = value / 100
                    }
                    Label {
                        text: Math.round((root.audioController ? root.audioController.volume : 1) * 100) + "%"
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: ExoTheme.fontCaption
                        color: ExoTheme.textSecondary
                    }
                }
            }
        }
    }
}
