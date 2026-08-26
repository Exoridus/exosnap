pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root

    required property RecordViewModelAdapter recordViewModel

    // Three groups, three weights. Left: which sources feed the recording, as
    // round peers. Centre: the elapsed time, the largest thing on the page.
    // Right: the secondary per-recording actions as round icons, then the one
    // recommended action as a filled pill. The previous dock put all eight
    // controls on the same rounded rectangle at the same size, which is why it
    // read as a developer toolbar rather than as a recorder's transport.
    //
    // Anchored, not laid out. The timer used to sit in a RowLayout between two
    // fill-spacers, which centres it between the two CLUSTERS rather than in the
    // bar — so it slid sideways every time a state change made one cluster wider
    // (Ready → Recording moved it by ~40 px). Anchoring it to the bar's own
    // horizontal centre makes the position a property of the bar instead of a
    // by-product of what is currently visible.
    //
    // The 860 px minimum window runs the transport one control rung down, and
    // with it every gap below. The action cluster is inherently the wider of the
    // two (four icons plus the Stop pill while recording), and the timer is
    // pinned to the bar's geometric centre rather than laid out between the
    // clusters — so what the compact rung protects is the lane the timer has
    // left. Widening the gaps at 860 closes it: measured, the shutter button
    // came to rest against the last digit.
    readonly property bool compactControls: !ExoTheme.isRegular(root.width)

    // Three gaps, three roles, and at the regular rung they are 16 / 12 / 16:
    // from a cluster to the bar's edge, between round peers inside a cluster,
    // and between the secondary cluster and the one recommended action. The edge
    // used to be the widest of the three at 24, on the theory that a round end
    // needs more air than a straight one — side by side that read as the
    // clusters having been pushed inwards, with the peers sitting tighter than
    // their own distance to nothing.
    readonly property int contentInsetX: ExoTheme.spacingLg
    readonly property int contentInsetY: ExoTheme.spacingMd
    readonly property int clusterSpacing: root.compactControls ? ExoTheme.spacingSm : ExoTheme.spacingMd
    // Between the secondary cluster and the recommended action, so that action
    // reads as its own tier rather than as a fifth icon. Expressed as the
    // difference, because the RowLayout has already applied `clusterSpacing` by
    // the time this margin is added.
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
    radius: height / 2

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
        spacing: root.clusterSpacing
        anchors {
            left: parent.left
            leftMargin: root.contentInsetX
            verticalCenter: parent.verticalCenter
        }

        RecordSourceToggle {
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
            visible: root.recordViewModel.appAudioVisible
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
            visible: root.recordViewModel.microphoneAvailable
            onClicked: root.recordViewModel.requestToggleSource("microphone")
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

    // ── Centre: the elapsed time ─────────────────────────────────────────────
    //
    // On the value rung with tabular figures: this is what the user looks at
    // while recording, and at 18 px it carried the same weight as the button
    // labels either side of it.
    Label {
        text: root.recordViewModel.countdownActive
              ? root.recordViewModel.countdownRemaining.toString()
              : root.recordViewModel.elapsedText.length > 0 ? root.recordViewModel.elapsedText : qsTr("0:00")
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        // Red while a recording is running, because that is the one state the
        // clock is counting something irreversible in. Paused used to be amber
        // here, which put caution on a number that is simply not advancing —
        // it takes the accent the Resume action beside it carries instead.
        color: root.recordViewModel.recording ? ExoTheme.error
                                              : root.recordViewModel.paused ? ExoTheme.accent : ExoTheme.text
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
        spacing: root.clusterSpacing
        anchors {
            right: parent.right
            rightMargin: root.contentInsetX
            verticalCenter: parent.verticalCenter
        }

        RecordActionButton {
            compact: root.compactControls
            accessibleLabel: qsTr("Capture frame")
            text: qsTr("Frame")
            glyph: ExoGlyph.Shutter
            available: root.recordViewModel.captureFrameEnabled
            // The screenshot is read back out of the live preview, so before the
            // preview has produced a frame there is nothing to capture. Saying so
            // is the difference between "broken" and "not yet".
            unavailableReason: qsTr("Unavailable — the preview has not produced a frame yet.")
            visible: !root.recordViewModel.preparing && !root.recordViewModel.finalizing
            onClicked: root.recordViewModel.requestCaptureFrame()
        }

        RecordActionButton {
            compact: root.compactControls
            accessibleLabel: qsTr("Add marker")
            text: qsTr("Mark")
            glyph: ExoGlyph.Flag
            visible: root.recordViewModel.recording || root.recordViewModel.paused
            onClicked: root.recordViewModel.requestAddMarker()
        }

        RecordActionButton {
            compact: root.compactControls
            accessibleLabel: qsTr("Split recording")
            text: qsTr("Split")
            glyph: ExoGlyph.Scissors
            available: root.recordViewModel.splitEnabled
            // Two real causes, and only the first is worth a sentence: a manual
            // split needs a Matroska container, or one is already in flight. The
            // second clears itself within a segment boundary.
            unavailableReason: qsTr("Unavailable — a manual split needs an MKV or WebM container.")
            visible: root.recordViewModel.recording || root.recordViewModel.paused
            onClicked: root.recordViewModel.requestSplit()
        }

        // Pause is a secondary action DURING a recording — Stop is the one that
        // ends it — so it stays a round peer of the three above.
        RecordActionButton {
            compact: root.compactControls

            accessibleLabel: qsTr("Pause recording")
            text: qsTr("Pause")
            glyph: ExoGlyph.Pause
            visible: root.recordViewModel.recording
            onClicked: root.recordViewModel.requestPause()
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

        // The one recommended action of the Completed state (ADR 0022). It takes
        // the accent pill that Record otherwise holds — after a recording
        // finishes, editing it is what the product is for, and starting the next
        // one is not. Record is not removed, only stepped down to a plain pill
        // beside it: hiding it would leave no way out of Completed except
        // dismissing the result.
        //
        // Hidden rather than disabled when the recording cannot be edited at all
        // (split recording, missing file, failed run) — a permanently dead button
        // next to a successful result reads as a defect.
        RecordActionButton {
            id: editButton

            compact: root.compactControls
            accessibleLabel: qsTr("Edit recording")
            text: qsTr("Edit")
            round: false
            emphasised: true
            emphasisColor: ExoTheme.accent
            emphasisTextColor: ExoTheme.accentInk
            visible: root.recordViewModel.canOpenEditor
            Layout.leftMargin: root.actionGap
            onClicked: root.recordViewModel.requestOpenEditor()
        }

        RecordSplitButton {
            id: primaryButton

            recordViewModel: root.recordViewModel
            compact: root.compactControls
            // Only one accent pill on the bar at a time. While Edit holds it,
            // Record keeps its split behaviour and its chevron and gives up the
            // emphasis.
            subdued: editButton.visible
            visible: !root.recordViewModel.recording && !root.recordViewModel.paused
            Layout.leftMargin: editButton.visible ? root.clusterSpacing : root.actionGap
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
