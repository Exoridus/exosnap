pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root

    required property RecordViewModelAdapter recordViewModel

    implicitHeight: 72
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.lineStrong
    radius: height / 2

    RowLayout {
        spacing: ExoTheme.spacingSm
        anchors {
            fill: parent
            leftMargin: ExoTheme.spacingLg
            rightMargin: ExoTheme.spacingLg
            topMargin: ExoTheme.spacingMd
            bottomMargin: ExoTheme.spacingMd
        }

        RowLayout {
            spacing: ExoTheme.spacingXs

            RecordSourceToggle {
                shortLabel: qsTr("SYS")
                accessibleLabel: qsTr("System audio")
                checkedState: root.recordViewModel.systemAudioEnabled
                meterLevel: root.recordViewModel.systemMeter
                enabled: root.recordViewModel.canSelectSource && !root.recordViewModel.blocked
                         && !root.recordViewModel.failed
                onClicked: root.recordViewModel.requestToggleSource("system")
            }

            RecordSourceToggle {
                shortLabel: qsTr("APP")
                accessibleLabel: qsTr("Application audio")
                checkedState: root.recordViewModel.appAudioEnabled
                meterLevel: root.recordViewModel.appMeter
                enabled: root.recordViewModel.canSelectSource && !root.recordViewModel.blocked
                         && !root.recordViewModel.failed
                visible: root.recordViewModel.appAudioVisible
                onClicked: root.recordViewModel.requestToggleSource("app")
            }

            RecordSourceToggle {
                shortLabel: qsTr("MIC")
                accessibleLabel: root.recordViewModel.microphoneAvailable
                                 ? qsTr("Microphone") : qsTr("No microphone connected")
                checkedState: root.recordViewModel.microphoneEnabled
                meterLevel: root.recordViewModel.microphoneMeter
                enabled: root.recordViewModel.canSelectSource && root.recordViewModel.microphoneAvailable
                         && !root.recordViewModel.blocked && !root.recordViewModel.failed
                onClicked: root.recordViewModel.requestToggleSource("microphone")
            }

            RecordSourceToggle {
                shortLabel: qsTr("CAM")
                accessibleLabel: root.recordViewModel.webcamError
                                 ? qsTr("Camera can't be opened — %1").arg(root.recordViewModel.webcamErrorText)
                                 : root.recordViewModel.webcamAvailable ? qsTr("Webcam")
                                                                        : qsTr("No camera connected")
                checkedState: root.recordViewModel.webcamEnabled
                errorState: root.recordViewModel.webcamError
                enabled: root.recordViewModel.webcamAvailable && !root.recordViewModel.finalizing
                         && !root.recordViewModel.blocked && !root.recordViewModel.failed
                onClicked: root.recordViewModel.requestToggleSource("webcam")
            }
        }

        Item { Layout.fillWidth: true }

        Label {
            text: root.recordViewModel.countdownActive
                  ? root.recordViewModel.countdownRemaining.toString()
                  : root.recordViewModel.elapsedText.length > 0 ? root.recordViewModel.elapsedText : qsTr("0:00")
            textFormat: Text.PlainText
            horizontalAlignment: Text.AlignHCenter
            color: root.recordViewModel.recording ? ExoTheme.error
                                                  : root.recordViewModel.paused ? ExoTheme.warning : ExoTheme.text
            Layout.minimumWidth: 70
            font {
                family: ExoTheme.monoFamily
                pixelSize: 18
                weight: Font.DemiBold
            }
        }

        Item { Layout.fillWidth: true }

        RowLayout {
            spacing: ExoTheme.spacingXs

            RecordActionButton {
                accessibleLabel: qsTr("Capture frame")
                text: qsTr("Frame")
                round: false
                enabled: root.recordViewModel.captureFrameEnabled
                visible: !root.recordViewModel.preparing && !root.recordViewModel.finalizing
                onClicked: root.recordViewModel.requestCaptureFrame()
            }

            RecordActionButton {
                accessibleLabel: qsTr("Add marker")
                text: qsTr("Mark")
                round: false
                visible: root.recordViewModel.recording || root.recordViewModel.paused
                onClicked: root.recordViewModel.requestAddMarker()
            }

            RecordActionButton {
                accessibleLabel: qsTr("Split recording")
                text: qsTr("Split")
                round: false
                enabled: root.recordViewModel.splitEnabled
                visible: root.recordViewModel.recording || root.recordViewModel.paused
                onClicked: root.recordViewModel.requestSplit()
            }

            RecordActionButton {
                id: pauseButton

                accessibleLabel: qsTr("Pause recording")
                text: qsTr("Pause")
                round: false
                visible: root.recordViewModel.recording
                onClicked: root.recordViewModel.requestPause()
            }

            RecordActionButton {
                id: resumeButton

                accessibleLabel: qsTr("Resume recording")
                text: qsTr("Resume")
                round: false
                emphasisColor: ExoTheme.accent
                emphasisTextColor: ExoTheme.accentInk
                visible: root.recordViewModel.paused
                onClicked: root.recordViewModel.requestResume()
            }

            RecordActionButton {
                id: stopButton

                accessibleLabel: qsTr("Stop recording")
                text: qsTr("Stop")
                round: false
                emphasisColor: ExoTheme.error
                emphasisTextColor: ExoTheme.errorInk
                visible: root.recordViewModel.recording || root.recordViewModel.paused
                onClicked: root.recordViewModel.requestStop()
            }

            ComboBox {
                id: countdownCombo

                readonly property var seconds: [0, 3, 5, 10]

                model: [qsTr("Now"), qsTr("3 s"), qsTr("5 s"), qsTr("10 s")]
                currentIndex: Math.max(0, countdownCombo.seconds.indexOf(root.recordViewModel.countdownSeconds))
                enabled: root.recordViewModel.canStart && !root.recordViewModel.countdownActive
                visible: !root.recordViewModel.recording && !root.recordViewModel.paused
                         && !root.recordViewModel.preparing && !root.recordViewModel.finalizing
                focusPolicy: Qt.StrongFocus
                Accessible.name: qsTr("Recording countdown")
                Layout.preferredWidth: 72
                onActivated: index => root.recordViewModel.requestCountdownSeconds(countdownCombo.seconds[index])
                palette {
                    button: ExoTheme.surfaceRaised
                    buttonText: ExoTheme.textSecondary
                    base: ExoTheme.surfaceRaised
                    window: ExoTheme.surfaceRaised
                    text: ExoTheme.textSecondary
                    highlight: ExoTheme.accent
                    highlightedText: ExoTheme.accentInk
                }

                contentItem: Label {
                    text: countdownCombo.displayText
                    textFormat: Text.PlainText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: countdownCombo.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: 12
                }

                background: Rectangle {
                    color: countdownCombo.down ? ExoTheme.surfaceHover : ExoTheme.surfaceRaised
                    border.width: countdownCombo.visualFocus ? 2 : 1
                    border.color: countdownCombo.visualFocus ? ExoTheme.text : ExoTheme.lineStrong
                    radius: ExoTheme.radiusSm
                }

                indicator: Label {
                    x: countdownCombo.width - width - 7
                    y: (countdownCombo.height - height) / 2
                    text: qsTr("⌄")
                    textFormat: Text.PlainText
                    color: countdownCombo.enabled ? ExoTheme.textMuted : ExoTheme.textDim
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: 12
                }
            }

            RecordActionButton {
                id: primaryButton

                accessibleLabel: root.recordViewModel.countdownActive ? qsTr("Cancel countdown")
                                 : root.recordViewModel.preparing ? qsTr("Preparing recording")
                                 : root.recordViewModel.finalizing ? qsTr("Finalizing recording")
                                                                   : qsTr("Start recording")
                text: root.recordViewModel.countdownActive ? qsTr("Cancel")
                      : root.recordViewModel.preparing ? qsTr("Preparing…")
                      : root.recordViewModel.finalizing ? qsTr("Finalizing…") : qsTr("Record")
                round: false
                emphasisColor: root.recordViewModel.countdownActive ? ExoTheme.error : ExoTheme.accent
                emphasisTextColor: ExoTheme.accentInk
                enabled: root.recordViewModel.countdownActive || root.recordViewModel.canStart
                visible: !root.recordViewModel.recording && !root.recordViewModel.paused
                onClicked: root.recordViewModel.requestStart()
            }
        }
    }

    Connections {
        target: root.recordViewModel

        function onStateTextChanged() {
            if (!root.recordViewModel.active)
                return
            Qt.callLater(() => {
                if (pauseButton.visible && pauseButton.enabled)
                    pauseButton.forceActiveFocus()
                else if (resumeButton.visible && resumeButton.enabled)
                    resumeButton.forceActiveFocus()
                else if (stopButton.visible && stopButton.enabled)
                    stopButton.forceActiveFocus()
                else if (primaryButton.visible && primaryButton.enabled)
                    primaryButton.forceActiveFocus()
            })
        }
    }
}
