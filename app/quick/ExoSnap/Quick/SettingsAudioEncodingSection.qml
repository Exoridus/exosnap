import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The shape of the recorded audio stream: how many channels, at what rate, and
// under which codec-specific controls. Separate from Audio sources because none
// of this changes what is captured -- it changes what the captured audio is
// turned into.
ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Audio encoding")
    subtitle: root.settings.audioEncodingSummary

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
        visible: root.settings.expertMode && root.settings.audioSampleRateRelevant
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
        info: qsTr("The audio device and the video capture run off different clocks and drift apart by a few parts per million. Slaving resamples audio onto the video clock so a multi-hour recording stays in sync. Turn it off only when you need bit-exact audio samples.")
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
}
