import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Audio")
    subtitle: root.settings.audioSummary

    SettingsAudioSourceRow {
        label: qsTr("Application audio")
        sourceEnabled: root.settings.appAudioEnabled
        separateTrack: root.settings.appAudioSeparate
        locked: root.settings.controlsLocked
        meterLevel: root.settings.appMeter
        stacked: root.stacked
        visible: root.settings.appAudioVisible
        Layout.fillWidth: true
        onSourceToggled: value => root.settings.appAudioEnabled = value
        onSeparateToggled: value => root.settings.appAudioSeparate = value
    }

    SettingsAudioSourceRow {
        label: qsTr("System audio")
        sourceEnabled: root.settings.systemAudioEnabled
        separateTrack: root.settings.systemAudioSeparate
        locked: root.settings.controlsLocked
        meterLevel: root.settings.systemMeter
        stacked: root.stacked
        showMergeOption: root.settings.appAudioVisible
        Layout.fillWidth: true
        onSourceToggled: value => root.settings.systemAudioEnabled = value
        onSeparateToggled: value => root.settings.systemAudioSeparate = value
    }

    SettingsAudioSourceRow {
        label: qsTr("Microphone")
        sourceEnabled: root.settings.microphoneEnabled
        separateTrack: root.settings.microphoneSeparate
        locked: root.settings.controlsLocked
        meterLevel: root.settings.microphoneMeter
        stacked: root.stacked
        Layout.fillWidth: true
        onSourceToggled: value => root.settings.microphoneEnabled = value
        onSeparateToggled: value => root.settings.microphoneSeparate = value
    }

    ExoSettingRow {
        label: qsTr("Microphone device")
        stacked: root.stacked
        Layout.fillWidth: true

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            ExoSelect {
                options: root.settings.microphoneDeviceOptions
                value: root.settings.microphoneDeviceId
                enabled: !root.settings.controlsLocked
                placeholderText: qsTr("(no microphone)")
                Layout.fillWidth: true
                Accessible.name: qsTr("Microphone device")
                onValueActivated: value => root.settings.microphoneDeviceId = value
            }

            ExoButton {
                text: qsTr("Rescan")
                enabled: !root.settings.controlsLocked
                onClicked: root.settings.rescanAudioDevices()
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Microphone channels")
        hint: qsTr("How stereo mic inputs are mapped to the recorded channel")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.micChannelModeOptions
            value: root.settings.micChannelMode
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Microphone channel mode")
            onValueActivated: value => root.settings.micChannelMode = value
        }
    }

    ExoSettingRow {
        label: qsTr("Microphone gain")
        hint: qsTr("Boost or cut the microphone level before encoding")
        stacked: root.stacked
        Layout.fillWidth: true

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            ExoSlider {
                from: -12
                to: 12
                stepSize: 1
                value: root.settings.micGainDb
                enabled: !root.settings.controlsLocked
                Layout.fillWidth: true
                Accessible.name: qsTr("Microphone gain")
                onMovedByUser: value => root.settings.micGainDb = value
            }

            Label {
                text: qsTr("%1 dB").arg(Math.round(root.settings.micGainDb))
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignRight
                color: ExoTheme.textSecondary
                Layout.preferredWidth: 52
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontSecondary
                }
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Audio bitrate")
        stacked: root.stacked
        visible: root.settings.audioBitrateRelevant
        Layout.fillWidth: true

        ExoNumberField {
            from: 32
            to: 510
            stepSize: 8
            suffix: qsTr("kbps")
            value: root.settings.audioBitrateKbps
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Audio bitrate")
            onValueCommitted: value => root.settings.audioBitrateKbps = value
        }
    }

    ExoSettingRow {
        label: qsTr("Channels")
        hint: qsTr("Stereo preserves L/R · Mono mixes both channels")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.audioChannelsOptions
            value: root.settings.audioChannels
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Audio channels")
            onValueActivated: value => root.settings.audioChannels = value
        }
    }

    ExoSettingRow {
        label: qsTr("Sample rate")
        stacked: root.stacked
        visible: root.settings.audioSampleRateRelevant
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.audioSampleRateOptions
            value: root.settings.audioSampleRate
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Audio sample rate")
            onValueActivated: value => root.settings.audioSampleRate = value
        }
    }

    ExoSettingRow {
        label: qsTr("Audio bit depth")
        stacked: root.stacked
        visible: root.settings.audioBitDepthRelevant
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.audioBitDepthOptions
            value: root.settings.audioBitDepth
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Audio bit depth")
            onValueActivated: value => root.settings.audioBitDepth = value
        }
    }

    ExoSettingRow {
        label: qsTr("FLAC compression")
        hint: qsTr("FLAC compression level (0 = fastest, 8 = smallest file)")
        stacked: root.stacked
        visible: root.settings.flacCompressionRelevant
        Layout.fillWidth: true

        ExoNumberField {
            from: 0
            to: 8
            value: root.settings.flacCompressionLevel
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("FLAC compression level")
            onValueCommitted: value => root.settings.flacCompressionLevel = value
        }
    }

    ExoSettingRow {
        label: qsTr("Brickwall limiter")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.limiterEnabled
            enabled: !root.settings.controlsLocked
            Accessible.name: qsTr("Brickwall limiter")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.limiterEnabled = value
        }
    }

    ExoSettingRow {
        label: qsTr("Opus frame duration")
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.opusControlsRelevant
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.opusFrameDurationOptions
            value: root.settings.opusFrameDuration
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Opus frame duration")
            onValueActivated: value => root.settings.opusFrameDuration = value
        }
    }

    ExoSettingRow {
        label: qsTr("Opus complexity")
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.opusControlsRelevant
        Layout.fillWidth: true

        ExoNumberField {
            from: 0
            to: 10
            value: root.settings.opusComplexity
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Opus complexity")
            onValueCommitted: value => root.settings.opusComplexity = value
        }
    }

    ExoSettingRow {
        label: qsTr("A/V clock slaving")
        stacked: root.stacked
        controlWidth: 60
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.clockSlavingEnabled
            enabled: !root.settings.controlsLocked
            Accessible.name: qsTr("Audio/video clock slaving")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.clockSlavingEnabled = value
        }
    }

    ExoSettingRow {
        label: qsTr("Microphone post-processing")
        hint: root.settings.micPostProcessingSummary
        stacked: root.stacked
        controlWidth: 100
        Layout.fillWidth: true

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

            Label {
                text: micPostProcessing.visible ? qsTr("Hide") : qsTr("Configure")
                textFormat: Text.PlainText
                color: ExoTheme.textSecondary
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                }
            }

            ExoChevron {
                // 0 = down (expanded), -90 = right (collapsed) - same convention
                // as ExoDisclosure's header chevron.
                direction: micPostProcessing.visible ? 0 : -90
                tone: ExoTheme.textMuted
                Layout.preferredWidth: 12

                Behavior on rotation {
                    NumberAnimation {
                        duration: ExoTheme.animMedium
                        easing.type: Easing.OutCubic
                    }
                }
            }

            TapHandler {
                onTapped: micPostProcessing.visible = !micPostProcessing.visible
            }
        }
    }

    SettingsMicDspGroup {
        id: micPostProcessing

        settings: root.settings
        stacked: root.stacked
        visible: false
        Layout.fillWidth: true
    }
}
