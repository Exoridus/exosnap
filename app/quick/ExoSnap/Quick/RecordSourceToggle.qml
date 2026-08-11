import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root

    required property string shortLabel
    required property string accessibleLabel
    property alias checkedState: root.checked
    property bool errorState: false
    property real meterLevel: 0

    // An ExoGlyph.Kind. Set, the toggle draws the icon; -1 falls back to
    // `shortLabel`.
    //
    // The transport used to spell these out as "SYS", "APP", "MIC", "CAM" in 10 px
    // mono. Enlarged beside the Widgets reference that reads as four abbreviations
    // rather than four controls: the two that were off looked like dimmed captions,
    // and nothing about "SYS" says sound. A speaker, a window, a microphone and a
    // camera are conventional enough to carry it, and the accessible name and the
    // tooltip still say the full thing.
    property int glyph: ExoGlyph.Invalid

    // One rung down at the 860 px minimum window, in step with the action
    // buttons opposite — see RecordActionButton for why the transport has a
    // compact rung at all.
    property bool compact: false

    // Round, like the Widgets transport's source buttons: circles read as one
    // group of peers, which is what these four are.
    implicitWidth: root.compact ? ExoTheme.controlHeight : ExoTheme.controlHeightLarge
    implicitHeight: root.implicitWidth
    text: root.shortLabel
    hoverEnabled: true
    checkable: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: root.accessibleLabel

    readonly property color _ink: !root.enabled ? ExoTheme.textDim
                                  : root.errorState ? ExoTheme.error
                                  : root.checkedState ? ExoTheme.accent : ExoTheme.textSecondary

    contentItem: Item {
        Label {
            anchors.centerIn: parent
            text: root.text
            textFormat: Text.PlainText
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            visible: root.glyph === ExoGlyph.Invalid
            color: root._ink
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontEyebrow
                weight: Font.DemiBold
            }
        }

        ExoGlyph {
            anchors.centerIn: parent
            kind: root.glyph
            color: root._ink
            visible: root.glyph !== ExoGlyph.Invalid
            width: 18
            height: 18
        }
    }

    // Rest, hover and press are the same surface at three depths — so hover has
    // to tint the fill this toggle actually HAS. It tinted `surfaceRaised`
    // unconditionally, which meant hovering an unchecked toggle jumped two steps
    // up to somewhere brighter than the checked state it was not in.
    readonly property color _fill: root.checkedState && root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface

    background: Rectangle {
        color: root.down ? ExoTheme.pressTint(root._fill)
             : root.hovered && root.enabled ? ExoTheme.hoverTint(root._fill) : root._fill
        border.width: 1
        border.color: root.visualFocus ? ExoTheme.text
                                       : root.errorState ? ExoTheme.error
                                                         : root.checkedState ? ExoTheme.accent : ExoTheme.line
        radius: height / 2

        // The live level, as an arc hugging the button's own edge rather than a
        // bar floating inside it — a circle has no bottom edge to sit a bar on.
        Canvas {
            id: meterArc

            readonly property real level: Math.max(0, Math.min(1, root.meterLevel))
            readonly property color ink: root.checkedState ? ExoTheme.accent : ExoTheme.textDim

            anchors.fill: parent
            visible: meterArc.level > 0

            onLevelChanged: requestPaint()
            onInkChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                if (meterArc.level <= 0)
                    return;
                ctx.strokeStyle = meterArc.ink;
                ctx.lineWidth = 2;
                ctx.lineCap = "round";
                ctx.beginPath();
                // Starts at the bottom and sweeps both ways, so a quiet source
                // shows a short mark under the icon instead of a lopsided one.
                const sweep = Math.PI * 0.9 * meterArc.level;
                ctx.arc(width / 2, height / 2, width / 2 - 2, Math.PI / 2 - sweep / 2, Math.PI / 2 + sweep / 2);
                ctx.stroke();
            }
        }
    }

    ToolTip.visible: root.hovered
    ToolTip.text: root.accessibleLabel
}
