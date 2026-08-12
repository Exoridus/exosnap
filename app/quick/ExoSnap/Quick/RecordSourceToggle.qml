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

            // The live level, as an arc hugging the button's own edge rather than a
            // bar floating inside it — a circle has no bottom edge to sit a bar on.
            Canvas {
                id: meterArc

                // Deliberately NOT gated on `available`. The toggles are locked
                // for the whole recording, which is precisely when the live
                // level is the thing being watched.
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
