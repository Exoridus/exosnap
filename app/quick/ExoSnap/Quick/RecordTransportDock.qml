pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQml.Models

Rectangle {
    id: root

    required property RecordViewModelAdapter recordViewModel
    required property RecordingClipAdapter clipAdapter
    property AudioRoutingAdapter routing: null
    property bool reviewAvailable: false
    property bool reviewCanEdit: false
    signal reviewEditRequested()
    readonly property bool useRoutedSources: root.routing !== null && !root.routing.legacy
    readonly property var displayedSources: !root.useRoutedSources ? []
                                            : root.routing.recordingActive ? root.routing.activeSources : root.routing.sources
    readonly property int visibleSourceCount: root.width < 1100 ? 2 : 3

    // Equal lanes on either side leave the timer at the bar's geometric centre.
    // Secondary actions enter the More menu before they can overlap that lane.
    readonly property bool compactControls: !ExoTheme.isRegular(root.width)

    // Keep outer, peer, and primary-action gaps independent as controls enter overflow.
    readonly property int contentInsetX: ExoTheme.spacingLg
    readonly property int contentInsetY: ExoTheme.spacingMd
    readonly property int clusterSpacing: root.compactControls ? ExoTheme.spacingSm : ExoTheme.spacingMd
    // RowLayout already accounts for clusterSpacing before this additional gap.
    readonly property int actionGap: (root.compactControls ? ExoTheme.spacingMd : ExoTheme.spacingLg)
                                     - root.clusterSpacing

    // The dock is the base and its controls sit ON it. The previous pass had it
    // the other way round — a `surfaceRaised` bar with `surface` controls — and
    // side by side that reads as eight dark holes punched into the transport
    // rather than as eight buttons: the thing meant to be pressed was the darkest
    // thing on the page. Dropping the bar to `surface` and lifting every control
    // to `surfaceRaised` inverts exactly that one relationship; the geometry,
    // the grouping and the gaps are untouched.
    //
    // Where the three states resolve is ExoTheme.dockFill / dockBorder / dockInk,
    // not here, because the source toggles and the action buttons are peers on
    // this bar and had drifted into three different treatments between them.
    implicitHeight: (root.compactControls ? ExoTheme.controlHeight : ExoTheme.controlHeightLarge)
                    + 2 * root.contentInsetY
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusSm

    // Why a source cannot be toggled right now, in the order the conditions are
    // actually evaluated below. Composed from the same state the `available`
    // bindings read, so the sentence can never disagree with the button.
    readonly property string sourceLockReason: !root.recordViewModel.canSelectSource
                                               ? qsTr("Unavailable — the capture setup is locked while a recording runs.")
                                               : root.recordViewModel.blocked
                                                 ? qsTr("Unavailable — Diagnostics is reporting a blocker.")
                                                 : root.recordViewModel.failed
                                                   ? qsTr("Unavailable — the last recording failed.") : ""

    // ── Left: the sources ────────────────────────────────────────────────────
    RowLayout {
        id: sourceActions
        spacing: root.clusterSpacing
        anchors {
            left: parent.left
            leftMargin: root.contentInsetX
            verticalCenter: parent.verticalCenter
        }

        RecordSourceToggle {
            visible: !root.useRoutedSources
            compact: root.compactControls
            shortLabel: qsTr("SYS")
            glyph: ExoGlyph.Speaker
            accessibleLabel: qsTr("System audio")
            checkedState: root.recordViewModel.systemAudioEnabled
            meterLevel: root.recordViewModel.systemMeter
            // A running session no longer locks this: the toggle becomes a live
            // mute, and the track keeps its full length either way.
            available: (root.recordViewModel.canSelectSource
                        || root.recordViewModel.liveToggleableSources.includes("system"))
                       && !root.recordViewModel.blocked && !root.recordViewModel.failed
            unavailableReason: root.sourceLockReason
            onClicked: root.recordViewModel.requestToggleSource("system")
        }

        RecordSourceToggle {
            compact: root.compactControls
            shortLabel: qsTr("APP")
            glyph: ExoGlyph.AppWindow
            accessibleLabel: qsTr("Application audio")
            checkedState: root.recordViewModel.appAudioEnabled
            meterLevel: root.recordViewModel.appMeter
            // A running session no longer locks this: the toggle becomes a live
            // mute, and the track keeps its full length either way.
            available: (root.recordViewModel.canSelectSource
                        || root.recordViewModel.liveToggleableSources.includes("app"))
                       && !root.recordViewModel.blocked && !root.recordViewModel.failed
            unavailableReason: root.sourceLockReason
            visible: !root.useRoutedSources && root.recordViewModel.appAudioVisible
            onClicked: root.recordViewModel.requestToggleSource("app")
        }

        RecordSourceToggle {
            compact: root.compactControls
            shortLabel: qsTr("MIC")
            glyph: ExoGlyph.Mic
            accessibleLabel: qsTr("Microphone")
            checkedState: root.recordViewModel.microphoneEnabled
            meterLevel: root.recordViewModel.microphoneMeter
            available: (root.recordViewModel.canSelectSource
                        || root.recordViewModel.liveToggleableSources.includes("microphone"))
                       && root.recordViewModel.microphoneAvailable
                       && !root.recordViewModel.blocked && !root.recordViewModel.failed
            // The device fact outranks the session lock: with no microphone
            // attached, "locked while a recording runs" would be true and
            // useless — plugging one in is what changes the answer.
            unavailableReason: root.sourceLockReason
            // No microphone attached at all: the control has nothing to offer and
            // no reason worth reading, so it is not shown -- the same rule the
            // application-audio toggle already follows.
            visible: !root.useRoutedSources && root.recordViewModel.microphoneAvailable
            onClicked: root.recordViewModel.requestToggleSource("microphone")
        }

        Repeater {
            model: root.displayedSources
            delegate: RecordSourceToggle {
                required property var modelData
                required property int index
                readonly property var liveMeter: root.routing.sourceMeters[modelData.id]
                visible: index < root.visibleSourceCount
                compact: root.compactControls
                shortLabel: modelData.name.length > 0 ? modelData.name.slice(0, 3).toUpperCase() : qsTr("AUD")
                glyph: ExoGlyph.Speaker
                accessibleLabel: qsTr("%1, %2, %3").arg(modelData.name)
                                 .arg(modelData.muted ? qsTr("muted") : qsTr("unmuted"))
                                 .arg(liveMeter && liveMeter.available ? qsTr("level available") : qsTr("level unavailable"))
                checkedState: modelData.participation && !modelData.muted
                meterLevel: liveMeter && liveMeter.available ? Math.pow(10, liveMeter.rmsDbfs / 20) : 0
                available: modelData.available && modelData.participation
                unavailableReason: !modelData.available ? modelData.availabilityText
                                   : !modelData.participation ? qsTr("Source is not participating")
                                   : !liveMeter || !liveMeter.available ? qsTr("Signal level unavailable") : ""
                onClicked: root.routing.setSourceMute(modelData.id, !modelData.muted, root.routing.revision)
            }
        }

        RecordActionButton {
            id: sourceOverflowAction
            compact: root.compactControls
            accessibleLabel: qsTr("More audio sources")
            text: qsTr("Sources")
            glyph: ExoGlyph.Overflow
            visible: root.useRoutedSources && root.displayedSources.length > root.visibleSourceCount
            onClicked: sourceOverflow.open()
        }

        RecordSourceToggle {
            compact: root.compactControls
            shortLabel: qsTr("CAM")
            glyph: ExoGlyph.Webcam
            accessibleLabel: qsTr("Webcam")
            checkedState: root.recordViewModel.webcamEnabled
            errorState: root.recordViewModel.webcamError
            meterLevel: 0
            available: root.recordViewModel.webcamAvailable && !root.recordViewModel.finalizing
                       && !root.recordViewModel.blocked && !root.recordViewModel.failed
            // No camera attached at all: not shown, like the microphone toggle.
            // A camera that IS attached and will not open stays visible -- device
            // presence is what this flag reports, and its error is something the
            // user can act on.
            visible: root.recordViewModel.webcamAvailable
            // The engine's own words when it has them. `webcamErrorText` is the
            // reason the camera would not open, which no generic sentence here
            // could improve on.
            unavailableReason: root.recordViewModel.webcamError
                               ? qsTr("Can't be opened — %1").arg(root.recordViewModel.webcamErrorText)
                               : root.recordViewModel.finalizing
                                   ? qsTr("Unavailable — the recording is still being written.")
                                   // The camera is not part of the capture setup
                                   // the recording lock covers, so it never
                                   // borrows the lock's sentence.
                                   : root.recordViewModel.blocked || root.recordViewModel.failed
                                     ? root.sourceLockReason : ""
            onClicked: root.recordViewModel.requestToggleSource("webcam")
        }
    }

    Menu {
        id: sourceOverflow
        x: sourceActions.x + sourceOverflowAction.x
        y: -height - ExoTheme.spacingSm

        Instantiator {
            model: root.displayedSources.length > root.visibleSourceCount
                   ? root.displayedSources.slice(root.visibleSourceCount) : []
            delegate: MenuItem {
                required property var modelData
                text: qsTr("%1: %2").arg(modelData.name)
                      .arg(!modelData.available ? modelData.availabilityText
                           : !modelData.participation ? qsTr("Not participating")
                           : modelData.muted ? qsTr("Muted") : qsTr("Unmuted"))
                enabled: modelData.available && modelData.participation
                onTriggered: root.routing.setSourceMute(modelData.id, !modelData.muted, root.routing.revision)
            }
            onObjectAdded: (index, object) => sourceOverflow.insertItem(index, object)
            onObjectRemoved: (index, object) => sourceOverflow.removeItem(object)
        }
    }

    // ── Centre: the elapsed time ─────────────────────────────────────────────
    //
    // On the value rung with tabular figures: this is what the user looks at
    // while recording, and at 18 px it carried the same weight as the button
    // labels either side of it.
    Label {
        id: elapsedTimer
        objectName: "recordTransportClock"
        text: root.recordViewModel.countdownActive
              ? root.recordViewModel.countdownRemaining.toString()
              : root.recordViewModel.elapsedText.length > 0 ? root.recordViewModel.elapsedText : qsTr("00:00:00")
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        width: Math.max(implicitWidth, 112)
        // Red while a recording is running, because that is the one state the
        // clock is counting something irreversible in. Paused used to be amber
        // here, which put caution on a number that is simply not advancing —
        // it takes the accent the Resume action beside it carries instead.
        color: root.recordViewModel.recording ? ExoTheme.error
                                              : root.recordViewModel.paused ? ExoTheme.paused : ExoTheme.text
        anchors {
            horizontalCenter: parent.horizontalCenter
            verticalCenter: parent.verticalCenter
        }
        font {
            family: ExoTheme.monoFamily
            // A countdown is one digit where every other state is an eight
            // character clock, and at the clock's size that digit read as a
            // stray number rather than as the thing the whole window is waiting
            // on. One rung up for the seconds that are counting down.
            pixelSize: root.recordViewModel.countdownActive ? ExoTheme.fontValueLarge : ExoTheme.fontValue
            weight: Font.DemiBold
            features: { "tnum": 1 }
        }
    }

    // ── Right: secondary actions, then the one recommended action ────────────
    RowLayout {
        id: rightActions
        objectName: "recordTransportActions"
        spacing: root.clusterSpacing
        anchors {
            right: parent.right
            rightMargin: root.contentInsetX
            verticalCenter: parent.verticalCenter
        }

        RecordActionButton {
            id: frameAction
            compact: root.compactControls
            accessibleLabel: qsTr("Capture frame")
            text: qsTr("Frame")
            glyph: ExoGlyph.Shutter
            available: root.recordViewModel.captureFrameEnabled
            // The screenshot is read back out of the live preview, so before the
            // preview has produced a frame there is nothing to capture. Saying so
            // is the difference between "broken" and "not yet".
            unavailableReason: qsTr("Unavailable — the preview has not produced a frame yet.")
            // Not beside a failed run: its preview is not what the user is being
            // asked to look at, and the bar's one question is the failure.
            visible: root.width >= 1100 && !root.recordViewModel.preparing && !root.recordViewModel.finalizing &&
                     !root.recordViewModel.failed
            onClicked: root.recordViewModel.requestCaptureFrame()
        }

        RecordActionButton {
            compact: root.compactControls
            accessibleLabel: qsTr("Add marker")
            text: qsTr("Mark")
            glyph: ExoGlyph.Flag
            visible: root.width >= 1600 && (root.recordViewModel.recording || root.recordViewModel.paused)
            onClicked: root.recordViewModel.requestAddMarker()
        }

        RecordActionButton {
            compact: root.compactControls
            accessibleLabel: qsTr("Split recording")
            text: qsTr("Split")
            glyph: ExoGlyph.SplitTrack
            available: root.recordViewModel.splitEnabled
            // Two real causes, and only the first is worth a sentence: a manual
            // split needs a Matroska container, or one is already in flight. The
            // second clears itself within a segment boundary.
            unavailableReason: qsTr("Unavailable — a manual split needs an MKV or WebM container.")
            visible: root.width >= 1600 && (root.recordViewModel.recording || root.recordViewModel.paused)
            onClicked: root.recordViewModel.requestSplit()
        }

        RecordActionButton {
            id: clipAction
            objectName: "recordClipAction"
            enabled: visible
            activeFocusOnTab: visible
            Accessible.ignored: !visible
            compact: root.compactControls
            accessibleLabel: qsTr("Save clip, %1 seconds selected").arg(root.clipAdapter.preferredDurationSeconds)
            text: qsTr("Clip · %1 s").arg(root.clipAdapter.preferredDurationSeconds)
            glyph: ExoGlyph.Scissors
            visible: root.clipAdapter.productAvailable && root.width >= 1100 && (root.recordViewModel.recording || root.recordViewModel.paused || root.clipAdapter.jobs.length > 0)
            available: root.clipAdapter.productAvailable && (root.clipAdapter.canRequest || root.clipAdapter.jobs.length > 0)
            unavailableReason: root.clipAdapter.unavailableReason
            onClicked: root.openClipPanel()
        }

        // Pause is a secondary action DURING a recording — Stop is the one that
        // ends it — so it stays a round peer of the three above.
        RecordActionButton {
            compact: root.compactControls

            accessibleLabel: qsTr("Pause recording")
            text: qsTr("Pause")
            glyph: ExoGlyph.Pause
            visible: root.width >= 1100 && root.recordViewModel.recording
            onClicked: root.recordViewModel.requestPause()
        }

        RecordActionButton {
            id: moreAction
            compact: root.compactControls
            accessibleLabel: qsTr("More recording actions")
            text: qsTr("More")
            glyph: ExoGlyph.Overflow
            visible: !root.recordViewModel.failed && (root.width < 1100 ||
                     (root.width < 1600 && (root.recordViewModel.recording || root.recordViewModel.paused ||
                                            root.recordViewModel.pendingEditMasterAvailable)))
            onClicked: moreMenu.open()
        }

        RecordActionButton {
            compact: root.compactControls

            accessibleLabel: qsTr("Resume recording")
            text: qsTr("Resume")
            round: false
            emphasised: true
            emphasisColor: ExoTheme.accent
            emphasisTextColor: ExoTheme.accentInk
            visible: root.recordViewModel.paused
            Layout.leftMargin: root.actionGap
            onClicked: root.recordViewModel.requestResume()
        }

        RecordActionButton {
            compact: root.compactControls

            accessibleLabel: qsTr("Stop recording")
            text: qsTr("Stop")
            round: false
            emphasised: true
            // Filled while recording — ending the recording IS the state's one
            // recommended action. Outlined while paused, where Resume holds that
            // role: a filled red pill next to a filled accent pill gives the bar
            // two primaries and no answer to "what now?".
            emphasisOutlined: root.recordViewModel.paused
            emphasisColor: ExoTheme.error
            emphasisTextColor: ExoTheme.errorInk
            visible: root.recordViewModel.recording || root.recordViewModel.paused
            Layout.leftMargin: root.recordViewModel.paused ? 0 : root.actionGap
            onClicked: root.recordViewModel.requestStop()
        }

        // The way out of a failed run. A failure stays on the bar until it is
        // acknowledged, so the Record action is withheld until then. A successful
        // run needs no such step: the Saved notification and Edit History carry
        // the result, and the bar is immediately ready for the next recording.
        //
        // Nothing is undone by it: whatever the run left on disk stays there.
        RecordActionButton {
            id: dismissButton

            compact: root.compactControls
            accessibleLabel: qsTr("Back to the transport")
            text: qsTr("Back")
            glyph: ExoGlyph.Back
            round: true
            visible: root.recordViewModel.failed
            onClicked: root.recordViewModel.requestDismissResult()
        }

        RecordSplitButton {
            id: primaryButton

            recordViewModel: root.recordViewModel
            compact: root.compactControls
            // Not shown beside a failure, which Back acknowledges first. After a
            // successful run this is the bar's action again straight away.
            visible: !root.recordViewModel.recording && !root.recordViewModel.paused &&
                     !root.recordViewModel.failed
            Layout.leftMargin: root.actionGap
        }
        RecordActionButton {
            compact: root.compactControls
            accessibleLabel: qsTr("Edit finalized recording")
            text: qsTr("Edit")
            round: !root.reviewAvailable
            glyph: root.reviewAvailable ? ExoGlyph.Invalid : ExoGlyph.ExternalLink
            visible: root.reviewAvailable || (root.width >= 1600 && root.recordViewModel.pendingEditMasterAvailable)
            available: !root.reviewAvailable || root.reviewCanEdit
            unavailableReason: qsTr("The completed recording is unavailable.")
            onClicked: {
                if (root.reviewAvailable)
                    root.reviewEditRequested();
                else
                    root.recordViewModel.requestEditMaster();
            }
        }

    }

    Menu {
        id: moreMenu
        x: Math.max(8, rightActions.x + moreAction.x - width + moreAction.width)
        y: -height - ExoTheme.spacingSm

        MenuItem {
            text: qsTr("Capture frame")
            visible: root.width < 1100 && !root.recordViewModel.preparing && !root.recordViewModel.finalizing
            enabled: root.recordViewModel.captureFrameEnabled
            onTriggered: root.recordViewModel.requestCaptureFrame()
        }
        MenuItem {
            text: qsTr("Add marker")
            visible: root.width < 1600 && (root.recordViewModel.recording || root.recordViewModel.paused)
            onTriggered: root.recordViewModel.requestAddMarker()
        }
        MenuItem {
            text: qsTr("Split recording")
            visible: root.width < 1600 && (root.recordViewModel.recording || root.recordViewModel.paused)
            enabled: root.recordViewModel.splitEnabled
            onTriggered: root.recordViewModel.requestSplit()
        }
        MenuItem {
            id: clipOverflowAction
            objectName: "recordClipOverflowAction"
            height: clipOverflowAction.visible ? clipOverflowAction.implicitHeight : 0
            text: qsTr("Save clip")
            activeFocusOnTab: visible && enabled
            Accessible.ignored: !visible
            visible: root.clipAdapter.productAvailable && root.width < 1100 && (root.recordViewModel.recording || root.recordViewModel.paused || root.clipAdapter.jobs.length > 0)
            enabled: root.clipAdapter.productAvailable && (root.clipAdapter.canRequest || root.clipAdapter.jobs.length > 0)
            onTriggered: root.openClipPanel()
        }
        MenuItem {
            text: qsTr("Edit finalized recording")
            visible: !root.reviewAvailable && root.width < 1600 && root.recordViewModel.pendingEditMasterAvailable
            onTriggered: root.recordViewModel.requestEditMaster()
        }
        MenuItem {
            text: qsTr("Pause recording")
            visible: root.width < 1100 && root.recordViewModel.recording
            onTriggered: root.recordViewModel.requestPause()
        }
    }

    function openClipPanel() {
        if (root.clipAdapter.productAvailable && clipPanelLoader.status === Loader.Ready)
            clipPanelLoader.item.open();
    }

    Loader {
        id: clipPanelLoader
        objectName: "recordClipPanelLoader"
        active: root.clipAdapter.productAvailable
        sourceComponent: RecordingClipPanel {
            clipAdapter: root.clipAdapter
            x: Math.max(8, root.width - width - 8)
            y: -height - ExoTheme.spacingSm
        }
    }

    // No focus is claimed here on a state change. Every recording state change
    // used to pull the active focus into whichever transport button the new
    // state recommends, which is wrong twice over: it took the keyboard away
    // from a user who was working somewhere else on the page, and — because a
    // recording can start, fail or finish while a modal surface is up — it took
    // it out of an open modal, whose whole contract is that it keeps the
    // keyboard until it is answered.
    //
    // Nothing depended on that focus. The transport's keyboard shortcuts are
    // process-wide hotkeys registered by Win32HotkeyRegistrar, not QML `Shortcut`
    // items scoped to a focused control, so they keep working with the focus
    // anywhere. Tab order into the dock is unchanged.
}
