pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Where in the recording the ledger's occurrences are (spec sections 5 and 7):
// a 6 px track for the whole file, mono ticks every 3 minutes plus the end
// time, and one mark per occurrence -- amber for a measured problem, coral for
// a real frame drop. Clicking anywhere on the track opens Edit at that time;
// hovering a mark names the problem, its clock, duration and worst value.
Item {
    id: root

    // The recording's own elapsed time: the clock the marks carry, and the span
    // the track is drawn over.
    required property int durationMs
    // The finished file's length. Shorter than `durationMs` by the tail between
    // the last encoded frame and Stop, so a mark that opened in that tail would
    // otherwise hand Edit a position past the end of the file. 0 means unknown
    // and clamps nothing.
    property int mediaDurationMs: 0
    // [{ startMs: int, durationMs: int, tone: string, title: string, worstText: string }]
    property var marks: []

    signal openAtRequested(int positionMs)

    implicitWidth: 300
    implicitHeight: track.height + 4 + ticks.height

    function _clockText(ms: int): string {
        const totalSeconds = Math.floor(ms / 1000);
        const minutes = Math.floor(totalSeconds / 60);
        const seconds = totalSeconds % 60;
        return minutes + ":" + (seconds < 10 ? "0" : "") + seconds;
    }

    function _durationText(ms: int): string {
        return (Math.round(ms / 100) / 10) + " " + qsTr("s");
    }

    // Every 3 minutes, plus one final tick at the true end. A 3-minute tick
    // less than one step away from the end is dropped rather than crowding a
    // second label ("12:00" then "12:34" 34 s later) next to it.
    readonly property var _ticks: {
        const stepMs = 180000;
        const result = [{ atMs: 0, label: root._clockText(0) }];
        for (let at = stepMs; at <= root.durationMs - stepMs; at += stepMs)
            result.push({ atMs: at, label: root._clockText(at) });
        if (root.durationMs > 0)
            result.push({ atMs: root.durationMs, label: root._clockText(root.durationMs) });
        return result;
    }

    // Where in the FILE a session position is. The recording outlives its last
    // encoded frame, so the tail of the track has no media behind it and opens
    // Edit at the end instead of past it.
    function _mediaPosition(positionMs: int): int {
        const clamped = Math.max(0, positionMs);
        return root.mediaDurationMs > 0 ? Math.min(clamped, root.mediaDurationMs) : clamped;
    }

    function _toneColor(tone: string): color {
        return tone === "critical" ? ExoTheme.error : ExoTheme.warning;
    }

    Rectangle {
        id: track

        objectName: "sessionTimelineTrack"
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
        }
        height: 6
        radius: 3
        color: ExoTheme.surfaceRaised

        // One handler for the whole track, rather than a second one per mark:
        // a mark sits on top of the track visually, and two overlapping
        // TapHandlers on the same point both fire, which reported a mark's
        // click one pixel's worth of time late. A tap inside a mark's own
        // span opens at that mark's exact start; everywhere else it opens at
        // the tapped position.
        TapHandler {
            onTapped: (eventPoint) => {
                // Same guard the mark and tick bindings carry: with no duration
                // every position is NaN, the mark loop matches nothing, and each
                // tap would silently open at 0 rather than at what was clicked.
                if (root.durationMs <= 0) {
                    root.openAtRequested(0);
                    return;
                }
                const x = eventPoint.position.x;
                for (let i = 0; i < root.marks.length; ++i) {
                    const m = root.marks[i];
                    const markX = track.width * m.startMs / root.durationMs;
                    const markWidth = Math.max(2, track.width * m.durationMs / root.durationMs);
                    if (x >= markX && x <= markX + markWidth) {
                        root.openAtRequested(root._mediaPosition(m.startMs));
                        return;
                    }
                }
                const ratio = Math.max(0, Math.min(1, x / track.width));
                root.openAtRequested(root._mediaPosition(Math.round(ratio * root.durationMs)));
            }
        }

        Repeater {
            model: root.marks

            Rectangle {
                id: mark

                required property var modelData

                objectName: "sessionTimelineMark"
                readonly property real _widthPx: root.durationMs > 0
                    ? Math.max(2, track.width * mark.modelData.durationMs / root.durationMs) : 2
                readonly property real _xPx: root.durationMs > 0
                    ? track.width * mark.modelData.startMs / root.durationMs : 0

                x: mark._xPx
                y: -3
                width: mark._widthPx
                height: 12
                radius: 2
                color: root._toneColor(mark.modelData.tone)

                Accessible.role: Accessible.StaticText
                Accessible.name: mark.modelData.title + ". " + root._clockText(mark.modelData.startMs)

                ToolTip.text: mark.modelData.title + " · " + root._clockText(mark.modelData.startMs)
                    + " · " + root._durationText(mark.modelData.durationMs)
                    + " · " + qsTr("worst %1").arg(mark.modelData.worstText)
                ToolTip.visible: markHover.hovered
                ToolTip.delay: 400

                HoverHandler {
                    id: markHover
                }
            }
        }
    }

    Item {
        id: ticks

        objectName: "sessionTimelineTicks"
        anchors {
            left: parent.left
            right: parent.right
            top: track.bottom
            topMargin: 4
        }
        height: 14

        Repeater {
            id: ticksRepeater

            model: root._ticks

            Label {
                id: tickLabel

                required property var modelData
                required property int index

                objectName: "sessionTimelineTickLabel"
                text: tickLabel.modelData.label
                textFormat: Text.PlainText
                color: ExoTheme.textDim
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontEyebrow
                    letterSpacing: 0.5
                }

                readonly property real _targetX: root.durationMs > 0
                    ? ticks.width * tickLabel.modelData.atMs / root.durationMs : 0

                // First tick reads from the left edge, last from the right edge;
                // every tick in between centres on its own position -- the same
                // rule the design canon's tick row uses.
                x: tickLabel.index === 0 ? 0
                    : tickLabel.index === ticksRepeater.count - 1 ? ticks.width - tickLabel.implicitWidth
                    : tickLabel._targetX - tickLabel.implicitWidth / 2
            }
        }
    }
}
