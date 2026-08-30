import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What goes into the recording, and where it comes from. Split from Audio
// encoding because the two answer different questions: this card is about which
// sources are captured and how the microphone is picked up, that one is about
// the shape of the resulting audio stream. Both are visible in Default; Expert
// only adds rows inside them.
ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Audio sources")
    subtitle: root.settings.audioSummary

    // Always listed, receding rather than disappearing while a window is not the
    // capture target: it is a persisted setting, and a row that vanishes teaches
    // the user that the application-audio option does not exist.
    SettingsAudioSourceRow {
        label: qsTr("Application audio")
        hint: root.settings.appAudioVisible
              ? ""
              : qsTr("Only while capturing a window")
        sourceEnabled: root.settings.appAudioEnabled
        separateTrack: root.settings.appAudioSeparate
        locked: root.settings.controlsLocked || !root.settings.appAudioVisible
        meterLevel: root.settings.appMeter
        meterDb: root.settings.appMeterDb
        stacked: root.stacked
        showMixOption: false
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
        meterDb: root.settings.systemMeterDb
        stacked: root.stacked
        showMixOption: root.settings.appAudioVisible
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
        meterDb: root.settings.microphoneMeterDb
        stacked: root.stacked
        Layout.fillWidth: true
        onSourceToggled: value => root.settings.microphoneEnabled = value
        onSeparateToggled: value => root.settings.microphoneSeparate = value
    }

    ExoSettingRow {
        label: qsTr("Microphone device")
        stacked: root.stacked
        Layout.fillWidth: true

        // No Rescan control beside it. Audio endpoints are discovered from
        // IMMNotificationClient, so the list is already current whenever it is
        // opened; a button that repeats what the system already reported bought
        // nothing and cost the select the width a real device name needs.
        ExoSelect {
            options: root.settings.microphoneDeviceOptions
            value: root.settings.microphoneDeviceId
            enabled: !root.settings.controlsLocked
            placeholderText: qsTr("(no microphone)")
            Layout.fillWidth: true
            Accessible.name: qsTr("Microphone device")
            onValueActivated: value => root.settings.microphoneDeviceId = value
        }
    }

    ExoSettingRow {
        label: qsTr("Microphone channels")
        info: qsTr("A stereo microphone can be recorded as it arrives, folded down to mono, or taken from one side only. Auto follows the device: a mono microphone stays mono and a stereo one stays stereo.")
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
                id: micGainSlider

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
                // Follows the slider: a readout at full strength beside a receded
                // track claims the value is still editable.
                color: micGainSlider.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
                Layout.preferredWidth: 52
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontSecondary
                }
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Microphone post-processing")
        hint: root.settings.micPostProcessingSummary
        stacked: root.stacked
        controlWidth: 100
        Layout.fillWidth: true

        Item {
            id: micPostProcessingToggle

            implicitWidth: toggleRow.implicitWidth
            implicitHeight: toggleRow.implicitHeight
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

            // Same pattern as ExoDisclosure's header: a plain Item stands in for
            // a button, so Tab order, Space activation and the Accessible role
            // that a real Control gets for free all need to be spelled out here.
            // Space only, not Enter - product-spec 10.1 reserves Enter for a
            // dialog's default button and gives every other control exactly one
            // activation key.
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Microphone post-processing")
            Accessible.checkable: true
            Accessible.checked: micPostProcessing.visible
            Accessible.focusable: true
            Accessible.focused: micPostProcessingToggle.activeFocus
            Accessible.onPressAction: micPostProcessingToggle.toggle()
            Accessible.onToggleAction: micPostProcessingToggle.toggle()

            function toggle(): void {
                micPostProcessing.visible = !micPostProcessing.visible;
            }

            Keys.onSpacePressed: event => {
                micPostProcessingToggle.toggle();
                event.accepted = true;
            }

            Rectangle {
                anchors.fill: parent
                radius: ExoTheme.radiusSm
                color: "transparent"
                border.width: micPostProcessingToggle.activeFocus ? ExoTheme.focusRingWidth : 0
                border.color: ExoTheme.text
            }

            RowLayout {
                id: toggleRow

                anchors.fill: parent
                spacing: ExoTheme.spacingSm

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
            }

            TapHandler {
                onTapped: micPostProcessingToggle.toggle()
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
