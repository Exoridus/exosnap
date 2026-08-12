import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root

    // Action weight, not colour: the caller names what the button MEANS and the
    // theme decides how that reads. Three rungs, which is exactly what the
    // Widgets overlays established and what Recovery, the recording-error
    // surface and the crash report each need:
    //   "neutral"     — the default; secondary/dismissive actions.
    //   "primary"     — the one recommended, safe action (accent-filled).
    //   "destructive" — deletes user data (error-tinted, never filled, so it can
    //                   never win a glance against the safe action next to it).
    property string tone: "neutral"
    property alias selected: root.checked
    property bool quiet: false
    property bool selectable: false
    // An ExoGlyph.Kind. Set, the button draws that icon in place of its label —
    // for the handful of actions whose whole name is a symbol (the preset
    // overflow). `text` stays as the accessible name's fallback.
    property int glyph: ExoGlyph.Invalid
    // One control rung down, for a button that lives inside chrome rather than
    // on a page: the Record page's preview toolbar is 38 px tall, and a
    // full-height button there leaves a 1 px margin and reads as the toolbar's
    // subject rather than as an action inside it.
    property bool compact: false

    readonly property bool _iconOnly: root.glyph !== ExoGlyph.Invalid

    readonly property bool _primary: root.tone === "primary" && root.enabled
    readonly property bool _destructive: root.tone === "destructive" && root.enabled

    // A quiet button carries no chrome at rest, so a minimum width would only pad
    // a text run with dead space; a chromed one needs the reserve so a row of
    // short labels does not turn into a row of differently-sized boxes.
    implicitWidth: root._iconOnly ? root.implicitHeight
                                  : Math.max(root.quiet || root.compact ? 0 : 88,
                                             contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: root.compact ? ExoTheme.controlHeightCompact
                                 : root.tone === "primary" ? ExoTheme.controlHeightLarge : ExoTheme.controlHeight
    leftPadding: root._iconOnly ? 0 : root.compact ? ExoTheme.spacingMd : ExoTheme.spacingLg
    rightPadding: root._iconOnly ? 0 : root.compact ? ExoTheme.spacingMd : ExoTheme.spacingLg
    topPadding: 0
    bottomPadding: 0
    // Explicitly off while disabled: a quiet action carries no chrome at rest,
    // so its hover fill is most of what says "this is a control" — and a
    // disabled control that still lights up under the pointer is telling the
    // user to press it.
    hoverEnabled: root.enabled
    checkable: root.selectable
    autoExclusive: root.selectable
    focusPolicy: Qt.StrongFocus

    readonly property color _ink: !root.enabled ? ExoTheme.textDim
                                  : root._primary ? ExoTheme.accentInk
                                  : root._destructive ? ExoTheme.error
                                  : root.selected ? ExoTheme.text : ExoTheme.textSecondary

    contentItem: Item {
        implicitWidth: root._iconOnly ? icon.width : buttonLabel.implicitWidth
        implicitHeight: root._iconOnly ? icon.height : buttonLabel.implicitHeight

        Label {
            id: buttonLabel

            anchors.fill: parent
            horizontalAlignment: root.quiet && root.width > root.implicitWidth ? Text.AlignLeft : Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: root.text
            textFormat: Text.PlainText
            elide: Text.ElideRight
            visible: !root._iconOnly
            color: root._ink
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontBody
                weight: root.selected || root._primary ? Font.DemiBold : Font.Medium
            }
        }

        ExoGlyph {
            id: icon

            anchors.centerIn: parent
            kind: root.glyph
            color: root._ink
            visible: root._iconOnly
            width: 18
            height: 18
        }
    }

    background: Rectangle {
        // The disabled rung applies to the CHROME, never to the label. A
        // disabled control keeps `textDim`, and `textDim` sits at the 3:1
        // product floor for an unavailable control (product-spec §8) with no
        // headroom at all — the palette was nudged twice to reach it. Fading the
        // whole control by `disabledOpacity` therefore does not dim the ink a
        // little, it takes it to ~1.6:1 in Dark and ~1.7:1 in Light and undoes
        // both of those nudges. Receding the fill and the border says
        // "unavailable" and costs the ink nothing; see the gate in
        // test_theme_contrast.cpp, which pins exactly this.
        opacity: root.enabled ? 1.0 : ExoTheme.disabledOpacity
        color: root._primary
               ? (root.down ? ExoTheme.pressTint(ExoTheme.accent) : root.hovered ? ExoTheme.hoverTint(ExoTheme.accent) : ExoTheme.accent)
               : root.down ? ExoTheme.surfaceHover
               : root.hovered && root.enabled ? ExoTheme.hoverTint(ExoTheme.surfaceRaised) : ExoTheme.surfaceRaised
        // A chromed button always shows its edge. Without it "Copy", "Export…" and
        // "Create support bundle" render as three text runs floating in a toolbar
        // — they read as links, and the one that actually builds a support bundle
        // reads no differently from a label next to it.
        border.width: root._primary && !root.visualFocus ? 0 : 1
        border.color: root.visualFocus ? ExoTheme.text
                      : root._destructive ? ExoTheme.error
                      : root.selected ? ExoTheme.accent : ExoTheme.lineStrong
        radius: ExoTheme.radiusSm
        visible: !root.quiet || root._primary || root._destructive || root.down || root.hovered || root.visualFocus || root.selected
    }
}
