import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root

    required property string accessibleLabel
    // The rest fill of a secondary round action. `surface` rather than
    // `surfaceRaised`, which is what RecordSourceToggle opposite it already used:
    // the transport dock is the raised thing, and its controls sit one step into
    // it. Overridden outright by the emphasised actions (Stop, Resume).
    property color emphasisColor: ExoTheme.surface
    property color emphasisTextColor: ExoTheme.textSecondary
    property bool round: true
    // An ExoGlyph.Kind. Set, the button draws the icon instead of its text; the
    // text stays as the tooltip and the accessible name.
    property int glyph: ExoGlyph.Invalid
    // The one recommended action in the transport (Record / Resume / Stop). It is
    // wider and bolder than the round secondary buttons beside it, because a row
    // in which every control is the same size has no hierarchy at all — which is
    // what the transport looked like next to the Widgets reference.
    property bool emphasised: false
    // One rung down at the 860 px minimum window. The timer is pinned to the
    // bar's geometric centre, which leaves it a lane of `width − 2 × the wider
    // cluster`; with 44 px controls the paused state (three round actions plus
    // Resume AND Stop) closes that lane to nothing and the cluster overlaps the
    // time. 36 is the shared control height, not a new number, and stays above
    // the desktop hit-target floor.
    property bool compact: false

    readonly property int _size: root.compact ? ExoTheme.controlHeight : ExoTheme.controlHeightLarge

    implicitWidth: root.round ? root._size
                              : Math.max(root.emphasised ? (root.compact ? 88 : 104) : (root.compact ? 72 : 84),
                                         contentItem.implicitWidth + 2 * ExoTheme.spacingLg)
    implicitHeight: root._size
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: root.accessibleLabel

    ToolTip.delay: 400

    contentItem: Item {
        implicitWidth: label.visible ? label.implicitWidth : glyphIcon.width
        implicitHeight: label.visible ? label.implicitHeight : glyphIcon.height

        Label {
            id: label

            anchors.centerIn: parent
            text: root.text
            textFormat: Text.PlainText
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            visible: root.glyph === ExoGlyph.Invalid
            color: root.enabled ? root.emphasisTextColor : ExoTheme.textDim
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
            color: root.enabled ? root.emphasisTextColor : ExoTheme.textDim
            visible: root.glyph !== ExoGlyph.Invalid
            width: 18
            height: 18
        }
    }

    background: Rectangle {
        // The theme's own hover/press rule rather than Qt.lighter/Qt.darker: on a
        // light theme the rest fill is already near-white, so lightening it on
        // hover did nothing at all.
        color: root.down ? ExoTheme.pressTint(root.emphasisColor)
                         : root.hovered && root.enabled ? ExoTheme.hoverTint(root.emphasisColor)
                                                       : root.emphasisColor
        border.width: root.visualFocus ? 2 : 1
        // Container-rung hairline, matching RecordSourceToggle. On the raised
        // dock the fill step already separates the control; a control-rung edge
        // on top of that is what made the transport read as outlined.
        border.color: root.visualFocus ? ExoTheme.text : ExoTheme.line
        radius: root.round ? height / 2 : ExoTheme.radiusPill
    }

    ToolTip.visible: root.hovered
    ToolTip.text: root.accessibleLabel
}
