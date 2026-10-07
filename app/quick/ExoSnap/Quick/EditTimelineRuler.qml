pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

Item {
    id: root
    required property real pixelsPerMs
    required property real viewportX
    required property real viewportWidth
    property real labelSpacing: 90
    readonly property real majorMs: majorInterval(pixelsPerMs, labelSpacing)
    readonly property real minorMs: majorMs / 5
    readonly property int firstTick: Math.max(0, Math.floor(viewportX / (minorMs * pixelsPerMs)))
    readonly property int tickCount: Math.ceil(viewportWidth / (minorMs * pixelsPerMs)) + 2
    implicitHeight: 32
    Accessible.ignored: true

    function majorInterval(scale: real, spacing: real): real {
        const intervals = [100, 200, 500, 1000, 2000, 5000, 10000, 15000,
                           30000, 60000, 120000, 300000, 600000, 1800000, 3600000];
        for (const interval of intervals) {
            if (interval * scale >= spacing)
                return interval;
        }
        return Math.ceil(spacing / (scale * 3600000)) * 3600000;
    }

    function label(milliseconds: real, interval: real): string {
        const totalSeconds = Math.floor(milliseconds / 1000);
        const minutes = Math.floor(totalSeconds / 60);
        const seconds = totalSeconds % 60;
        let result = String(minutes).padStart(2, "0") + ":" + String(seconds).padStart(2, "0");
        if (interval < 1000)
            result += "." + String(Math.round(milliseconds % 1000)).padStart(3, "0");
        return result;
    }

    Rectangle {
        anchors.fill: parent
        color: ExoTheme.surface
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: ExoTheme.line
    }
    Repeater {
        model: root.tickCount
        delegate: Item {
            required property int index
            readonly property int tick: root.firstTick + index
            readonly property bool major: tick % 5 === 0
            x: tick * root.minorMs * root.pixelsPerMs
            height: root.height
            Rectangle {
                anchors.bottom: parent.bottom
                width: 1
                height: parent.major ? 9 : 4
                color: parent.major ? ExoTheme.textMuted : ExoTheme.lineStrong
            }
            Label {
                x: 4
                y: 2
                text: root.label(parent.tick * root.minorMs, root.majorMs)
                visible: parent.major
                color: ExoTheme.textSecondary
                font.family: ExoTheme.monoFamily
                font.pixelSize: ExoTheme.fontCaption
            }
        }
    }
}
