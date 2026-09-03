import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Which sources go into the recording, and onto which track.
//
// The list follows the capture target, because the sources themselves do: App
// captures one process tree and Sys captures everything except that tree, so a
// display target has no App row to offer at all and its Sys row is the full
// system output. The card names the target above the list, which is what turns a
// list that is two rows long here and three rows long there into a consequence
// of a stated fact rather than a block that comes and goes.
//
// The microphone's own settings live in their own card. The row here answers
// "is it recorded"; that card answers "how".
ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Audio sources")
    subtitle: root.settings.audioTargetSummary

    // Always listed, receding rather than disappearing while a window is not the
    // capture target: it is a persisted setting, and a row that vanishes teaches
    // the user that the application-audio option does not exist.
    SettingsAudioSourceRow {
        label: qsTr("Application audio")
        hint: root.settings.appAudioVisible ? root.settings.appAudioHint
                                            : qsTr("Only while capturing a window")
        sourceEnabled: root.settings.appAudioEnabled
        separateTrack: root.settings.appAudioSeparate
        locked: root.settings.controlsLocked || !root.settings.appAudioVisible
        meterLevel: root.settings.appMeter
        meterDb: root.settings.appMeterDb
        stacked: root.stacked
        showMixOption: false
        showGain: root.settings.expertMode
        gainDb: root.settings.appGainDb
        Layout.fillWidth: true
        onSourceToggled: value => root.settings.appAudioEnabled = value
        onGainCommitted: value => root.settings.appGainDb = value
        onSeparateToggled: value => root.settings.appAudioSeparate = value
    }

    Rectangle {
        color: ExoTheme.line
        Layout.fillWidth: true
        Layout.preferredHeight: 1
    }

    SettingsAudioSourceRow {
        label: qsTr("System audio")
        // The one row whose MEANING moves with the target: everything, or
        // everything except the captured process. The label is stable so the row
        // keeps its identity; this line says which recording it is today.
        hint: root.settings.systemAudioHint
        sourceEnabled: root.settings.systemAudioEnabled
        separateTrack: root.settings.systemAudioSeparate
        locked: root.settings.controlsLocked
        meterLevel: root.settings.systemMeter
        meterDb: root.settings.systemMeterDb
        showGain: root.settings.expertMode
        gainDb: root.settings.systemGainDb
        onGainCommitted: value => root.settings.systemGainDb = value
        stacked: root.stacked
        showMixOption: root.settings.appAudioVisible
        Layout.fillWidth: true
        onSourceToggled: value => root.settings.systemAudioEnabled = value
        onSeparateToggled: value => root.settings.systemAudioSeparate = value
    }

    Rectangle {
        color: ExoTheme.line
        Layout.fillWidth: true
        Layout.preferredHeight: 1
    }

    SettingsAudioSourceRow {
        label: qsTr("Microphone")
        hint: root.settings.microphoneConnected ? "" : qsTr("No microphone connected")
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

    // A statement, not a disabled control. What cannot be done here is reached
    // from somewhere else, and a sentence can appear and disappear without the
    // card losing a control the user was reaching for.
    Label {
        text: qsTr("To record one application on a track of its own, capture its window instead.")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        visible: !root.settings.appAudioVisible
        color: ExoTheme.textMuted
        Layout.fillWidth: true
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontCaption
        }
    }

    SettingsAudioTrackLedger {
        tracks: root.settings.audioTrackRows
        Layout.fillWidth: true
    }
}
