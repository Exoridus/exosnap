import QtQuick
import QtQuick.Controls.Basic

// A transport-dock action: a round secondary control, or the one recommended
// action as a wider filled pill.
//
// A FocusScope around the Button rather than the Button itself, and `available`
// rather than `enabled` — see RecordSourceToggle for why: a disabled QML control
// receives no hover, so the tooltip that would say why the action is unavailable
// is the one tooltip that could never appear.
FocusScope {
    id: root

    required property string accessibleLabel
    // Why the action cannot be taken right now, as a sentence. Never invented —
    // the caller passes what the adapter actually knows.
    property string unavailableReason: ""
    property bool available: true
    property string text: ""
    property bool round: true
    // An ExoGlyph.Kind. Set, the button draws the icon instead of its text; the
    // text stays as the tooltip and the accessible name.
    property int glyph: ExoGlyph.Invalid
    // The one recommended action in the transport (Record / Resume / Stop). It is
    // wider and bolder than the round secondary buttons beside it, because a row
    // in which every control is the same size has no hierarchy at all — which is
    // what the transport looked like next to the Widgets reference.
    property bool emphasised: false
    // The fill and ink of an emphasised action. Ignored by the round secondary
    // buttons, which take the shared dock treatment instead.
    property color emphasisColor: ExoTheme.accent
    property color emphasisTextColor: ExoTheme.accentInk
    // An emphasised action that keeps its size and its colour but gives up the
    // fill, so it cannot win a glance against a filled action beside it. This is
    // what Stop becomes while a recording is paused: Resume is the state's one
    // recommended action, and two filled pills side by side is two primaries.
    // The same rule ExoButton's "destructive" tone already follows.
    property bool emphasisOutlined: false
    // One rung down at the 860 px minimum window. The timer is pinned to the
    // bar's geometric centre, which leaves it a lane of `width − 2 × the wider
    // cluster`; with 44 px controls the paused state (three round actions plus
    // Resume AND Stop) closes that lane to nothing and the cluster overlaps the
    // time. 36 is the shared control height, not a new number, and stays above
    // the desktop hit-target floor.
    property bool compact: false

    readonly property int _size: root.compact ? ExoTheme.controlHeight : ExoTheme.controlHeightLarge
    readonly property bool hovered: hover.hovered

    signal clicked()

    implicitWidth: root.round ? root._size
                              : Math.max(root.emphasised ? (root.compact ? 88 : 104) : (root.compact ? 72 : 84),
                                         button.contentItem.implicitWidth + 2 * ExoTheme.spacingLg)
    implicitHeight: root._size

    Accessible.role: Accessible.Button
    Accessible.name: root.accessibleLabel
    Accessible.description: root.available ? "" : root.unavailableReason
    // Without this the control announces itself as a button, advertises a UIA
    // Invoke pattern (Qt derives that from the role) and then does nothing when
    // it is invoked: the inner Button carries the click handler but is
    // Accessible.ignored, so assistive technology reaches this FocusScope and
    // finds no press action behind it. A screen-reader user could focus Pause,
    // Stop, Split, Add marker and Capture frame and never activate any of them.
    // Guarded on `available` for the same reason the inner Button is disabled:
    // an unavailable action must not be activatable by any input path.
    Accessible.onPressAction: {
        if (root.available)
            root.clicked();
    }

    HoverHandler {
        id: hover
    }

    Button {
        id: button

        anchors.fill: parent
        enabled: root.available
        focus: true
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        text: root.text
        Accessible.ignored: true
        onClicked: root.clicked()

        // An emphasised action carries its own colour outright; a round secondary
        // one takes the shared dock treatment, so every peer on the bar reads the
        // same way in every state.
        readonly property color ink: !root.emphasised
                                     ? ExoTheme.dockInk(root.available, false, false, root.hovered)
                                     : !root.available ? ExoTheme.textDim
                                     : root.emphasisOutlined ? root.emphasisColor : root.emphasisTextColor

        contentItem: Item {
            implicitWidth: label.visible ? label.implicitWidth : glyphIcon.width
            implicitHeight: label.visible ? label.implicitHeight : glyphIcon.height

            Label {
                id: label

                anchors.centerIn: parent
                text: button.text
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                visible: root.glyph === ExoGlyph.Invalid
                color: button.ink
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: root.emphasised ? ExoTheme.fontSectionTitle : ExoTheme.fontBody
                    weight: Font.DemiBold
                }
            }

            ExoGlyph {
                id: glyphIcon

                anchors.centerIn: parent
                kind: root.glyph
                color: button.ink
                visible: root.glyph !== ExoGlyph.Invalid
                width: 18
                height: 18
            }
        }

        background: Rectangle {
            color: !root.emphasised
                   ? ExoTheme.dockFill(root.available, false, root.hovered && root.available, button.down)
                   : !root.available ? ExoTheme.surface
                   : root.emphasisOutlined
                     ? (button.down ? ExoTheme.surfaceHover
                        : root.hovered ? ExoTheme.hoverTint(ExoTheme.surfaceRaised) : ExoTheme.surfaceRaised)
                   : button.down ? ExoTheme.pressTint(root.emphasisColor)
                   : root.hovered ? ExoTheme.hoverTint(root.emphasisColor) : root.emphasisColor
            border.width: button.visualFocus ? 2 : 1
            border.color: !root.emphasised
                          ? ExoTheme.dockBorder(root.available, false, root.hovered && root.available, false,
                                                button.visualFocus)
                          : button.visualFocus ? ExoTheme.text
                          : !root.available ? ExoTheme.line
                          : root.emphasisOutlined ? root.emphasisColor : "transparent"
            radius: root.round ? height / 2 : ExoTheme.radiusPill
        }
    }

    ToolTip {
        parent: root
        visible: root.hovered
        delay: 400
        text: root.unavailableReason.length === 0
              ? root.accessibleLabel
              : qsTr("%1\n%2").arg(root.accessibleLabel).arg(root.unavailableReason)
    }
}
