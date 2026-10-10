import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// How the microphone is picked up. Whether it is recorded at all is the source
// card's question, one card above.
//
// Its own card rather than a group inside Audio sources: the four rows are
// details of one device, and a card boundary states that without an indent. The
// labels drop the word "Microphone" because the card title already carries it --
// it stood four times in a row purely to make up for structure that was missing.
ExoCard {
    id: root

    function focusControl(target: int): var { return microphoneControl; }

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Microphone")
    // The card states the missing device once, here, instead of leaving four
    // greyed rows to imply it.
    subtitle: root.settings.microphoneSummary

    ExoSettingRow {
        label: qsTr("Device")
        stacked: root.stacked
        Layout.fillWidth: true

        // No Rescan control beside it. Audio endpoints are discovered from
        // IMMNotificationClient, so the list is already current whenever it is
        // opened; a button that repeats what the system already reported bought
        // nothing and cost the select the width a real device name needs.
        ExoSelect {
            id: microphoneControl
            options: root.settings.microphoneDeviceOptions
            value: root.settings.microphoneDeviceId
            enabled: !root.settings.controlsLocked && root.settings.microphoneConnected
            // Only a pinned device that is not plugged in matches no entry; an
            // unpinned one is the System default entry.
            placeholderText: root.settings.microphoneConnected ? qsTr("(not connected)")
                                                               : qsTr("No microphone found")
            Layout.fillWidth: true
            Accessible.name: qsTr("Microphone device")
            onValueActivated: value => root.settings.microphoneDeviceId = value
        }
    }

    Label {
        text: qsTr("Connect a microphone and it appears here on its own.")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        visible: !root.settings.microphoneConnected
        color: ExoTheme.textMuted
        Layout.fillWidth: true
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontCaption
        }
    }

    ExoSettingRow {
        label: qsTr("Channels")
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
        label: qsTr("Gain")
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
        label: qsTr("Post-processing")
        hint: root.settings.micPostProcessingSummary
        stacked: root.stacked
        controlWidth: 100
        Layout.fillWidth: true

        SettingsGroupToggle {
            id: micPostProcessingToggle

            accessibleName: qsTr("Microphone post-processing")
        }
    }

    // Built when first opened rather than kept hidden: the group stays
    // collapsed most of the time, and its rows are page-load work nobody sees.
    Loader {
        active: micPostProcessingToggle.open
        visible: active
        Layout.fillWidth: true

        sourceComponent: SettingsMicDspGroup {
            settings: root.settings
            stacked: root.stacked
        }
    }
}
