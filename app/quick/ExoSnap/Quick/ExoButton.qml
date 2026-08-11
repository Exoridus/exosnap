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

    readonly property bool _iconOnly: root.glyph !== ExoGlyph.Invalid

    readonly property bool _primary: root.tone === "primary" && root.enabled
    readonly property bool _destructive: root.tone === "destructive" && root.enabled

    // A quiet button carries no chrome at rest, so a minimum width would only pad
    // a text run with dead space; a chromed one needs the reserve so a row of
    // short labels does not turn into a row of differently-sized boxes.
    implicitWidth: root._iconOnly ? ExoTheme.controlHeight
                                  : Math.max(root.quiet ? 0 : 88,
                                             contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: root.tone === "primary" ? ExoTheme.controlHeightLarge : ExoTheme.controlHeight
    leftPadding: root._iconOnly ? 0 : ExoTheme.spacingLg
    rightPadding: root._iconOnly ? 0 : ExoTheme.spacingLg
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true
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
