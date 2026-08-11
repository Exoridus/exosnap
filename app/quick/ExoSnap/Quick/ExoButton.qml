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

    readonly property bool _primary: root.tone === "primary" && root.enabled
    readonly property bool _destructive: root.tone === "destructive" && root.enabled

    implicitWidth: Math.max(84, contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: ExoTheme.controlHeight
    leftPadding: ExoTheme.spacingMd
    rightPadding: ExoTheme.spacingMd
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true
    checkable: root.selectable
    autoExclusive: root.selectable
    focusPolicy: Qt.StrongFocus

    contentItem: Label {
        horizontalAlignment: root.quiet && root.width > root.implicitWidth ? Text.AlignLeft : Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: root.text
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: !root.enabled ? ExoTheme.textDim
                             : root._primary ? ExoTheme.accentInk
                             : root._destructive ? ExoTheme.error
                             : root.selected ? ExoTheme.text : ExoTheme.textSecondary
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
            weight: root.selected || root._primary ? Font.DemiBold : Font.Medium
        }
    }

    background: Rectangle {
        color: root._primary
               ? (root.down ? ExoTheme.pressTint(ExoTheme.accent) : root.hovered ? ExoTheme.hoverTint(ExoTheme.accent) : ExoTheme.accent)
               : root.down ? ExoTheme.surfaceHover
               : root.hovered && root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: root.visualFocus || root.selected || root._destructive ? 1 : 0
        border.color: root.visualFocus ? ExoTheme.text : root._destructive ? ExoTheme.error : ExoTheme.accent
        radius: ExoTheme.radiusSm
        visible: !root.quiet || root._primary || root._destructive || root.down || root.hovered || root.visualFocus || root.selected
    }
}
