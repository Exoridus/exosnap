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

    // A raised dock with recessed controls, not a flat bar with outlined ones.
    // The two clusters had drifted apart: the source toggles sat on `surface`
    // with a hairline, the action buttons on `surfaceRaised` with a full-strength
    // one, on a bar that was itself `surface` — so eight round peers on one bar
    // carried three different treatments and only the outlines told them apart.
    // Lifting the BAR one step and dropping every control onto `surface` gives
    // the same relationship in all four themes and lets the hairline go quiet.
    implicitHeight: (root.compactControls ? ExoTheme.controlHeight : ExoTheme.controlHeightLarge)
                    + 2 * root.contentInsetY
    color: ExoTheme.surfaceRaised
    border.width: 1
    border.color: ExoTheme.line
    radius: height / 2

    // ── Left: the sources ────────────────────────────────────────────────────
    RowLayout {
        id: sourceCluster

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
            enabled: root.recordViewModel.canSelectSource && !root.recordViewModel.blocked
                     && !root.recordViewModel.failed
            onClicked: root.recordViewModel.requestToggleSource("system")
        }

        RecordSourceToggle {
            compact: root.compactControls
            shortLabel: qsTr("APP")
            glyph: ExoGlyph.AppWindow
            accessibleLabel: qsTr("Application audio")
            checkedState: root.recordViewModel.appAudioEnabled
            meterLevel: root.recordViewModel.appMeter
            enabled: root.recordViewModel.canSelectSource && !root.recordViewModel.blocked
                     && !root.recordViewModel.failed
            visible: root.recordViewModel.appAudioVisible
            onClicked: root.recordViewModel.requestToggleSource("app")
        }

        RecordSourceToggle {
            compact: root.compactControls
            shortLabel: qsTr("MIC")
            glyph: ExoGlyph.Mic
            accessibleLabel: root.recordViewModel.microphoneAvailable
                             ? qsTr("Microphone") : qsTr("No microphone connected")
            checkedState: root.recordViewModel.microphoneEnabled
            meterLevel: root.recordViewModel.microphoneMeter
            enabled: root.recordViewModel.canSelectSource && root.recordViewModel.microphoneAvailable
                     && !root.recordViewModel.blocked && !root.recordViewModel.failed
            onClicked: root.recordViewModel.requestToggleSource("microphone")
        }

        RecordSourceToggle {
            compact: root.compactControls
            shortLabel: qsTr("CAM")
            glyph: ExoGlyph.Camera
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
        color: root.recordViewModel.recording ? ExoTheme.error
                                              : root.recordViewModel.paused ? ExoTheme.warning : ExoTheme.text
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
        id: actionCluster

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
            enabled: root.recordViewModel.captureFrameEnabled
            visible: !root.recordViewModel.preparing && !root.recordViewModel.finalizing
            ToolTip.visible: hovered
            ToolTip.text: accessibleLabel
            onClicked: root.recordViewModel.requestCaptureFrame()
        }

        RecordActionButton {
            compact: root.compactControls
            accessibleLabel: qsTr("Add marker")
            text: qsTr("Mark")
            glyph: ExoGlyph.Flag
            visible: root.recordViewModel.recording || root.recordViewModel.paused
            ToolTip.visible: hovered
            ToolTip.text: accessibleLabel
            onClicked: root.recordViewModel.requestAddMarker()
        }

        RecordActionButton {
            compact: root.compactControls
            accessibleLabel: qsTr("Split recording")
            text: qsTr("Split")
            glyph: ExoGlyph.Scissors
            enabled: root.recordViewModel.splitEnabled
            visible: root.recordViewModel.recording || root.recordViewModel.paused
            ToolTip.visible: hovered
            ToolTip.text: accessibleLabel
            onClicked: root.recordViewModel.requestSplit()
        }

        // Pause is a secondary action DURING a recording — Stop is the one that
        // ends it — so it stays a round peer of the three above.
        RecordActionButton {
            id: pauseButton

            compact: root.compactControls

            accessibleLabel: qsTr("Pause recording")
            text: qsTr("Pause")
            glyph: ExoGlyph.Pause
            visible: root.recordViewModel.recording
            ToolTip.visible: hovered
            ToolTip.text: accessibleLabel
            onClicked: root.recordViewModel.requestPause()
        }

        RecordActionButton {
            id: resumeButton

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
            id: stopButton

            compact: root.compactControls

            accessibleLabel: qsTr("Stop recording")
            text: qsTr("Stop")
            round: false
            emphasised: true
            emphasisColor: ExoTheme.error
            emphasisTextColor: ExoTheme.errorInk
            visible: root.recordViewModel.recording || root.recordViewModel.paused
            Layout.leftMargin: root.recordViewModel.paused ? 0 : root.actionGap
            onClicked: root.recordViewModel.requestStop()
        }

        RecordSplitButton {
            id: primaryButton

            recordViewModel: root.recordViewModel
            compact: root.compactControls
            visible: !root.recordViewModel.recording && !root.recordViewModel.paused
            Layout.leftMargin: root.actionGap
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
                else if (primaryButton.visible)
                    primaryButton.forceActiveFocus()
            })
        }
    }
}
