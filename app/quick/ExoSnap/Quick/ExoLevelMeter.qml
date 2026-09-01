pragma ComponentBehavior: Bound

import QtQuick

// Segmented mono level meter. `level` is the 0..1 position the adapter derives
// from the reading in dBFS; this item performs no signal maths of its own.
//
// Segments rather than one continuous bar: a filled bar is the shape this
// product uses for progress, and a level that looks like progress invites the
// reading "nearly done" instead of "nearly clipping". Discrete cells also give
// the eye something to count, which is what makes a glance enough.
Item {
    id: root

    required property real level
    property bool active: true

    // Zone edges on the METER scale, matching the Record dock's ring exactly.
    // The same loudness must not be amber on one surface and accent on the
    // other, and the two are read minutes apart by the same person.
    readonly property real cautionAt: 0.72
    readonly property real alarmAt: 0.9

    // Sixteen suits the narrow slot in the Record dock. A meter that spans a
    // whole card raises it: cell width is what decides legibility, so more width
    // has to buy more cells rather than fatter ones.
    property int segmentCount: 16
    readonly property real _clamped: Math.max(0, Math.min(1, root.level))
    // A segment lights when the level reaches its own lower edge, so the count
    // of lit cells is the reading. Rounding to nearest would light a cell the
    // signal has not got to yet.
    readonly property int _litCount: Math.floor(root._clamped * root.segmentCount)

    // Peak hold. The loudest cell of the last kPeakHoldMs, kept so a transient
    // that is over before the eye arrives is still readable. It decays rather
    // than being cleared: an indicator that vanishes cannot be told apart from
    // one that never fired.
    property int peakSegment: -1
    readonly property int _peakHoldMs: 1400

    implicitWidth: 80
    implicitHeight: 12
    Accessible.ignored: true

    onLevelChanged: {
        if (root._litCount > root.peakSegment) {
            root.peakSegment = root._litCount;
            peakHold.restart();
        }
    }

    onActiveChanged: {
        if (!root.active) {
            root.peakSegment = -1;
            peakHold.stop();
        }
    }

    Timer {
        id: peakHold

        interval: root._peakHoldMs
        // Steps down one cell at a time instead of dropping to the current
        // level: a peak that falls in one jump reads as a glitch, and the decay
        // is what shows the direction the signal is moving in.
        repeat: true
        onTriggered: {
            if (root.peakSegment <= root._litCount) {
                root.peakSegment = -1;
                peakHold.stop();
                return;
            }
            root.peakSegment -= 1;
            peakHold.interval = 120;
        }
        onRunningChanged: {
            if (peakHold.running)
                peakHold.interval = root._peakHoldMs;
        }
    }

    function _segmentInk(index: int): color {
        const position = index / root.segmentCount;
        if (!root.active)
            return ExoTheme.textDim;
        if (position >= root.alarmAt)
            return ExoTheme.error;
        if (position >= root.cautionAt)
            return ExoTheme.warning;
        return ExoTheme.accent;
    }

    // The frame is what makes an EMPTY meter visible. Without it the unlit cells
    // of an inactive source are the card's own colour, so a source that is off
    // has no meter at all rather than a quiet one -- and "off" and "on but
    // silent" become the same picture.
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        radius: 3
        border.width: 1
        border.color: root.active ? ExoTheme.lineStrong : ExoTheme.line
    }

    Row {
        anchors.fill: parent
        anchors.margins: 2
        spacing: 1

        Repeater {
            model: root.segmentCount

            Rectangle {
                id: segment

                required property int index

                readonly property bool lit: segment.index < root._litCount
                readonly property bool peak: segment.index === root.peakSegment && !segment.lit

                // The row's spacing is taken out of the cell rather than added
                // to it, so the meter ends exactly where its slot does however
                // many segments it has.
                width: (root.width - 4 - (root.segmentCount - 1)) / root.segmentCount
                height: root.height - 4
                radius: 1
                // The unlit track still says whether the source is live: at
                // silence an active meter and an inactive one would otherwise be
                // the same grey, and "on but quiet" is not "off".
                // The unlit track is a fill, not an outline. Outlined cells at
                // this size read as a dashed rule, and twenty-two of them read
                // as a decoration rather than as a scale.
                color: segment.lit || segment.peak ? root._segmentInk(segment.index)
                     : root.active ? ExoTheme.surfaceHover : ExoTheme.surface
                opacity: segment.peak ? 0.5 : 1.0
            }
        }
    }
}
