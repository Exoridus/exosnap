pragma ComponentBehavior: Bound

import QtQuick

// Trim timeline of the Edit surface: a decoded thumbnail strip, one label-only
// row per audio track, marker verticals, two trim handles and a playhead.
//
// Plain declarative QML on purpose. The measured data scale is ~8-20 tiles
// (width-driven, not duration-driven), 0-3 audio rows, and markers already
// thinned to one per pixel column in C++ — a custom QQuickItem or a Canvas would
// cost more than it saves at that size. No Qt Quick Controls anywhere in the
// handles: a Control brings focus, hover and background machinery to something
// that is a rectangle with a drag.
//
// Audio rows carry a label and a fill and nothing else. A peak envelope over a
// multi-hour recording means decoding the entire soundtrack, and an approximated
// one is the invented shape the product spec forbids.
Item {
    id: root

    required property EditSessionAdapter session
    required property EditTimelineAdapter timeline
    required property EditPlayerAdapter player

    readonly property int sideInset: 5 // keeps the centred handles/knob unclipped at 0%/100%
    readonly property int labelZoneHeight: 22
    readonly property int timeRowGap: 6
    readonly property int timeRowHeight: 14
    readonly property int stackHeight: root.timeline.videoRowHeight + root.timeline.audioStackHeight
    readonly property int trackX: root.sideInset
    readonly property int trackWidth: Math.max(root.width - 2 * root.sideInset, 0)
    readonly property bool interactive: root.session.durationMs > 0

    // View-only drag feedback. The authoritative trim range lives once, snapped
    // and in microseconds, on the session adapter; these two only exist between
    // press and release so the handle can follow the pointer before the snap.
    property string dragTarget: ""
    property real dragStartMs: 0
    property real dragEndMs: 0

    readonly property real shownTrimStartMs: root.dragTarget === "" ? root.session.trimStartMs : root.dragStartMs
    readonly property real shownTrimEndMs: root.dragTarget === "" ? root.session.trimEndMs : root.dragEndMs
    readonly property bool trimmed: root.interactive && (root.shownTrimStartMs > 0 || root.shownTrimEndMs < root.session.durationMs)

    function xForMs(ms: real): real {
        if (root.session.durationMs <= 0) {
            return root.trackX;
        }
        return root.trackX + ms * root.trackWidth / root.session.durationMs;
    }

    function msForX(x: real): real {
        if (root.trackWidth <= 0 || root.session.durationMs <= 0) {
            return 0;
        }
        return Math.round((x - root.trackX) * root.session.durationMs / root.trackWidth);
    }

    implicitHeight: root.labelZoneHeight + root.stackHeight + root.timeRowGap + root.timeRowHeight
    onTrackWidthChanged: root.timeline.trackWidth = root.trackWidth
    Component.onCompleted: root.timeline.trackWidth = root.trackWidth

    // ---- Row stack ----
    Rectangle {
        id: track

        x: root.trackX
        y: root.labelZoneHeight
        width: root.trackWidth
        height: root.stackHeight
        color: ExoTheme.surfaceRaised
        border.width: 1
        // The panel around this component now carries the workspace boundary, so
        // the track keeps only the hairline that separates it from the panel's
        // own fill — `lineStrong` here made two competing edges 8 px apart.
        border.color: ExoTheme.line
        radius: ExoTheme.radiusMd
        // The rows, the dim bands and the markers all have to stop at the
        // rounded shape; the handles and the playhead deliberately do not.
        clip: true

        // Video row: decoded tiles, each drawn left-aligned at its own timestamp.
        // A position without a tile stays empty — a placeholder pattern would be
        // information the clip never gave.
        Repeater {
            model: root.timeline.tileModel

            Image {
                id: tile

                required property string tileSource
                required property real timeMs

                x: root.xForMs(tile.timeMs) - root.trackX
                y: 0
                width: root.timeline.tileWidth
                height: root.timeline.videoRowHeight
                source: tile.tileSource
                sourceSize.width: root.timeline.tileWidth
                sourceSize.height: root.timeline.videoRowHeight
                asynchronous: true
                cache: false
                fillMode: Image.PreserveAspectCrop
            }
        }

        // Audio rows: a label over a fill.
        Repeater {
            model: root.timeline.audioTrackLabels

            Item {
                id: audioRow

                required property int index
                required property string modelData

                x: 0
                y: root.timeline.videoRowHeight + audioRow.index * (2 + root.timeline.audioRowHeight) + 2
                width: track.width
                height: root.timeline.audioRowHeight

                Rectangle {
                    anchors.fill: parent
                    color: Qt.alpha(ExoTheme.accent, 0.14)
                }

                // Hairline against the row above, so the lanes read as separate
                // tracks rather than as one tinted block.
                Rectangle {
                    height: 1
                    color: ExoTheme.lineStrong
                    anchors {
                        left: parent.left
                        right: parent.right
                        bottom: parent.top
                    }
                }

                Rectangle {
                    x: audioLabel.x - 3
                    y: audioLabel.y - 1
                    width: audioLabel.width + 6
                    height: audioLabel.height + 2
                    radius: 3
                    // Markers cross every row, so a marker near the start would
                    // otherwise strike through the track name.
                    color: Qt.alpha(ExoTheme.background, 0.6)
                }

                Text {
                    id: audioLabel

                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: audioRow.modelData
                    textFormat: Text.PlainText
                    color: ExoTheme.textMuted
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: ExoTheme.fontEyebrow
                    }
                }
            }
        }

        // Trimmed-away ranges are dimmed across every row: a trim applies to the
        // whole clip, not to one of its tracks.
        Rectangle {
            x: 0
            y: 0
            width: Math.max(root.xForMs(root.shownTrimStartMs) - root.trackX, 0)
            height: track.height
            visible: root.trimmed
            color: Qt.alpha(ExoTheme.background, 0.66)
        }

        Rectangle {
            id: trimTailDim

            x: root.xForMs(root.shownTrimEndMs) - root.trackX
            y: 0
            width: Math.max(track.width - trimTailDim.x, 0)
            height: track.height
            visible: root.trimmed
            color: Qt.alpha(ExoTheme.background, 0.66)
        }

        // Markers: thin verticals across the full stack. Studio Mint stays
        // reserved for the active trim handles and the caution colour for a real
        // diagnostic warning — a cut marker is neither. The Quick palette carries
        // no secondary accent, so a quiet neutral stands in.
        Repeater {
            model: root.timeline.markerModel

            Rectangle {
                id: markerLine

                required property real timeMs
                required property string label

                x: root.xForMs(markerLine.timeMs) - root.trackX - 1
                y: 0
                width: 2
                height: track.height
                visible: root.interactive
                color: Qt.alpha(ExoTheme.textSecondary, 0.85)

                Accessible.role: Accessible.Indicator
                Accessible.name: markerLine.label
            }
        }
    }

    // ---- Trim handles ----
    Repeater {
        model: root.interactive ? ["start", "end"] : []

        Rectangle {
            id: handle

            required property string modelData

            readonly property bool active: root.dragTarget === handle.modelData
            readonly property real handleMs: handle.modelData === "start" ? root.shownTrimStartMs : root.shownTrimEndMs

            x: root.xForMs(handle.handleMs) - width / 2
            y: track.y - 2
            width: handle.active ? 10 : 8
            height: track.height + 4
            radius: 4
            color: ExoTheme.accent
            border.width: 2
            border.color: ExoTheme.background

            Accessible.role: Accessible.Slider
            Accessible.name: handle.modelData === "start" ? qsTr("Trim in point") : qsTr("Trim out point")
        }
    }

    // ---- Playhead ----
    Item {
        visible: root.interactive
        x: root.xForMs(root.session.positionMs)
        y: track.y - 3

        Rectangle {
            x: -1
            y: 0
            width: 2
            height: track.height + 6
            color: ExoTheme.text
        }

        Rectangle {
            id: playheadKnob

            readonly property int knob: root.dragTarget === "playhead" ? 12 : 10

            x: -playheadKnob.knob / 2
            y: -playheadKnob.knob / 2
            width: playheadKnob.knob
            height: playheadKnob.knob
            radius: playheadKnob.knob / 2
            color: ExoTheme.text
        }
    }

    // ---- Drag feedback: a centred mono time label above the active element ----
    Rectangle {
        id: dragPill

        readonly property real labelMs: root.dragTarget === "start" ? root.shownTrimStartMs : root.dragTarget === "end" ? root.shownTrimEndMs : root.session.positionMs

        x: Math.max(0, Math.min(root.width - width, root.xForMs(dragPill.labelMs) - width / 2))
        y: 2
        width: dragLabel.implicitWidth + 12
        height: 16
        radius: 6
        visible: root.dragTarget !== ""
        color: ExoTheme.background
        border.width: 1
        border.color: ExoTheme.lineStrong

        Text {
            id: dragLabel

            anchors.centerIn: parent
            text: root.session.formatTimestamp(dragPill.labelMs)
            textFormat: Text.PlainText
            color: ExoTheme.text
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontEyebrow
            }
        }
    }

    // ---- Loading hint ----
    // While the video row has fewer tiles than the current width can hold, say
    // so — a strip that is merely mid-decode otherwise reads as broken. No
    // spinner and no skeleton tiles: a missing tile stays empty, deliberately.
    Text {
        x: root.trackX
        height: root.labelZoneHeight
        verticalAlignment: Text.AlignVCenter
        text: qsTr("Generating previews…")
        textFormat: Text.PlainText
        visible: root.interactive && root.dragTarget === "" && root.timeline.generatingPreviews
        color: ExoTheme.textDim
        font {
            family: ExoTheme.monoFamily
            pixelSize: ExoTheme.fontEyebrow
        }
    }

    // ---- Static time row: clip start · in/out readout while trimmed · duration ----
    Item {
        x: root.trackX
        y: track.y + track.height + root.timeRowGap
        width: root.trackWidth
        height: root.timeRowHeight

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.session.formatClock(0)
            textFormat: Text.PlainText
            color: ExoTheme.textDim
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontEyebrow
            }
        }

        Text {
            anchors.centerIn: parent
            text: qsTr("in %1 · out %2").arg(root.session.formatClock(root.shownTrimStartMs)).arg(root.session.formatClock(root.shownTrimEndMs))
            textFormat: Text.PlainText
            visible: root.trimmed
            color: ExoTheme.textDim
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontEyebrow
            }
        }

        Text {
            anchors {
                right: parent.right
                verticalCenter: parent.verticalCenter
            }
            text: root.session.formatClock(root.session.durationMs)
            textFormat: Text.PlainText
            color: ExoTheme.textDim
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontEyebrow
            }
        }
    }

    // ---- Interaction ----
    // One area over the whole stack plus the knob's overhang above it. The
    // handles beat the playhead on a tie: a handle is the harder target and the
    // playhead can be moved anywhere on the track.
    MouseArea {
        id: scrubArea

        readonly property int hitSlop: 4

        function hitTest(x: real): string {
            if (!root.interactive) {
                return "";
            }
            const halfWidth = (root.dragTarget === "" ? 8 : 10) / 2 + scrubArea.hitSlop;
            if (Math.abs(x - root.xForMs(root.shownTrimStartMs)) <= halfWidth) {
                return "start";
            }
            if (Math.abs(x - root.xForMs(root.shownTrimEndMs)) <= halfWidth) {
                return "end";
            }
            return "playhead";
        }

        // An enclosing Flickable would otherwise steal the drag mid-trim.
        preventStealing: true
        hoverEnabled: true
        cursorShape: root.interactive ? (scrubArea.hitTest(mouseX) === "playhead" ? Qt.PointingHandCursor : Qt.SizeHorCursor) : Qt.ArrowCursor
        x: 0
        y: track.y - 10
        width: root.width
        height: track.height + 10

        onPressed: mouse => {
            if (!root.interactive) {
                return;
            }
            root.dragStartMs = root.session.trimStartMs;
            root.dragEndMs = root.session.trimEndMs;
            root.dragTarget = scrubArea.hitTest(mouse.x);
            if (root.dragTarget === "playhead") {
                // Press on the track jumps the playhead AND begins the scrub in
                // one event, rather than requiring a second drag.
                root.player.beginScrub();
                root.session.requestSeek(root.msForX(mouse.x));
            }
        }

        onPositionChanged: mouse => {
            if (root.dragTarget === "") {
                return;
            }
            const ms = root.msForX(mouse.x);
            if (root.dragTarget === "start") {
                root.dragStartMs = root.session.clampTrimStartMs(ms, root.dragEndMs);
            } else if (root.dragTarget === "end") {
                root.dragEndMs = root.session.clampTrimEndMs(ms, root.dragStartMs);
            } else {
                root.session.requestSeek(ms);
            }
        }

        onReleased: {
            if (root.dragTarget === "playhead") {
                root.player.endScrub();
            } else if (root.dragTarget !== "") {
                // Snapping happens once, here, on the session adapter: nearest
                // keyframe at or before the release point, then a marker within
                // the snap window.
                root.session.requestTrim(root.dragStartMs, root.dragEndMs);
            }
            root.dragTarget = "";
        }

        onCanceled: {
            if (root.dragTarget === "playhead") {
                root.player.endScrub();
            }
            root.dragTarget = "";
        }
    }
}
