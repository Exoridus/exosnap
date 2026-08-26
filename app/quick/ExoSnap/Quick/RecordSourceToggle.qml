import QtQuick
import QtQuick.Controls.Basic

// One audio/video source in the transport dock.
//
// A FocusScope around the Button rather than the Button itself, and `available`
// rather than `enabled`, for one reason: a disabled QML control receives no
// hover, so the tooltip that would explain WHY it is unavailable is exactly the
// one that could never appear. The scope stays enabled and carries the hover
// handler and the tooltip; the Button inside is genuinely disabled, so it cannot
// be activated by mouse or keyboard and reports the real accessible state.
FocusScope {
    id: root

    required property string shortLabel
    required property string accessibleLabel
    // Why this source cannot be used right now, as a sentence. Shown under the
    // label in the tooltip while `available` is false, and never invented: the
    // caller passes what the adapter actually knows.
    property string unavailableReason: ""
    property bool available: true
    property bool checkedState: false
    property bool errorState: false
    property real meterLevel: 0

    // An ExoGlyph.Kind. Set, the toggle draws the icon; Invalid falls back to
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

    readonly property string text: root.shortLabel
    readonly property bool hovered: hover.hovered

    signal clicked()

    // Round, like the Widgets transport's source buttons: circles read as one
    // group of peers, which is what these four are.
    implicitWidth: root.compact ? ExoTheme.controlHeight : ExoTheme.controlHeightLarge
    implicitHeight: root.implicitWidth

    Accessible.role: Accessible.CheckBox
    Accessible.name: root.accessibleLabel
    Accessible.description: root.available ? "" : root.unavailableReason

    HoverHandler {
        id: hover

        cursorShape: root.available ? Qt.PointingHandCursor : Qt.ArrowCursor
    }

    Button {
        id: button

        anchors.fill: parent
        enabled: root.available
        focus: true
        hoverEnabled: true
        checkable: true
        checked: root.checkedState
        focusPolicy: Qt.StrongFocus
        text: root.shortLabel
        // Spoken by the scope above, which is the item that stays enabled.
        Accessible.ignored: true
        onClicked: root.clicked()

        readonly property color ink: ExoTheme.dockInk(root.available, root.checkedState, root.errorState, root.hovered)

        contentItem: Item {
            Label {
                anchors.centerIn: parent
                text: button.text
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                visible: root.glyph === ExoGlyph.Invalid
                color: button.ink
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontEyebrow
                    weight: Font.DemiBold
                }
            }

            ExoGlyph {
                anchors.centerIn: parent
                kind: root.glyph
                color: button.ink
                visible: root.glyph !== ExoGlyph.Invalid
                width: 18
                height: 18
            }
        }

        background: Rectangle {
            color: ExoTheme.dockFill(root.available, root.checkedState, root.hovered && root.available, button.down)
            border.width: 1
            border.color: ExoTheme.dockBorder(root.available, root.checkedState, root.hovered && root.available,
                                              root.errorState, button.visualFocus)
            radius: height / 2

            // The live level as the button's own ground, rising from the bottom.
            // It is the control's background rather than a mark drawn on top, so
            // it never competes with the glyph for the same pixels.
            Canvas {
                id: meterFill

                // Deliberately NOT gated on `available`. The toggles can be locked
                // for a recording, which is precisely when the live level is the
                // thing being watched.
                readonly property real level: Math.max(0, Math.min(1, root.meterLevel))
                // Zone edges on the METER scale, not on the drawn height: the
                // colour at a given loudness must not move as the fill grows,
                // otherwise the same level reads differently from one frame to the
                // next.
                readonly property real cautionAt: 0.72
                readonly property real alarmAt: 0.9
                readonly property color baseInk: root.checkedState ? ExoTheme.accent : ExoTheme.textDim
                readonly property color cautionInk: ExoTheme.warning
                readonly property color alarmInk: ExoTheme.error

                anchors.fill: parent
                visible: meterFill.level > 0

                onLevelChanged: requestPaint()
                onBaseInkChanged: requestPaint()
                onCautionInkChanged: requestPaint()
                onAlarmInkChanged: requestPaint()

                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    if (meterFill.level <= 0)
                        return;

                    // Clipped to the control's own circle: a straight band would
                    // square off the corners of a round button.
                    ctx.beginPath();
                    ctx.arc(width / 2, height / 2, Math.min(width, height) / 2 - 1, 0, 2 * Math.PI);
                    ctx.clip();

                    // Denser than the resting background (dockFill tints at 0.22)
                    // so the level reads as a level, and still transparent enough
                    // that the border and glyph stay the control's identity.
                    const band = function (from, to, tone) {
                        if (meterFill.level <= from)
                            return;
                        const top = Math.min(meterFill.level, to);
                        ctx.fillStyle = Qt.rgba(tone.r, tone.g, tone.b, 0.38);
                        ctx.fillRect(0, height * (1 - top), width, height * (top - from));
                    };

                    band(0, meterFill.cautionAt, meterFill.baseInk);
                    band(meterFill.cautionAt, meterFill.alarmAt, meterFill.cautionInk);
                    band(meterFill.alarmAt, 1, meterFill.alarmInk);
                }
            }
        }
    }

    // The reason belongs on its own line under the name: "Webcam" then
    // "Unavailable — no camera was detected." reads as one control with a
    // status, where a single run reads as a long control name.
    ToolTip {
        parent: root
        visible: root.hovered
        delay: 400
        // The second line appears whenever there IS one — a camera that is
        // detected but will not open is available to click and still owes the
        // user the reason it is showing an error ring.
        text: root.unavailableReason.length === 0
              ? root.accessibleLabel
              : qsTr("%1\n%2").arg(root.accessibleLabel).arg(root.unavailableReason)
    }
}
