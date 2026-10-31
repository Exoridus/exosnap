import QtQuick
import QtQuick.Controls.Basic

ExoButton {
    id: root
    property string shortcutText: ""
    property string disabledReason: ""
    property bool panelAccess: false
    property bool expanded: false
    property bool pinned: false
    // Overrides the glyph's state colour, e.g. a warning glyph in error text.
    property string glyphColor: ""
    compact: true
    focusPolicy: Qt.TabFocus
    quiet: tone !== "primary"
    implicitWidth: 32
    implicitHeight: 32
    leftPadding: 0
    rightPadding: 0
    Accessible.name: text
    Accessible.checkable: root.checkable
    Accessible.checked: root.checked
    Accessible.expandable: panelAccess
    Accessible.expanded: panelAccess && expanded
    Accessible.description: !enabled && disabledReason.length > 0 ? disabledReason
        : panelAccess ? (pinned ? qsTr("Pinned panel") : expanded ? qsTr("Open panel") : qsTr("Closed panel")) : ""
    ToolTip.text: (shortcutText.length > 0 ? qsTr("%1 (%2)").arg(text).arg(shortcutText) : text)
        + (!enabled && disabledReason.length > 0 ? "\n" + disabledReason : "")
    ToolTip.visible: hovered || visualFocus || disabledHover.hovered
    ToolTip.delay: hovered || disabledHover.hovered ? 500 : 0
    HoverHandler {
        id: disabledHover
        enabled: !root.enabled && root.disabledReason.length > 0
    }
    readonly property bool activeState: checkable && checked || panelAccess && expanded
    contentItem: Item {
        ExoGlyph {
            anchors.centerIn: parent
            width: 18; height: 18
            kind: root.glyph
            visible: root.glyph !== ExoGlyph.Invalid
            color: !root.enabled ? ExoTheme.textDim : root.glyphColor.length > 0 ? root.glyphColor : root.tone === "primary" ? ExoTheme.accentInk : root.activeState ? ExoTheme.accent : ExoTheme.textSecondary
        }
        Label {
            anchors.fill: parent
            visible: root.glyph === ExoGlyph.Invalid
            text: root.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: root.enabled ? ExoTheme.text : ExoTheme.textDim
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontCaption
        }
    }
    background: Rectangle {
        radius: ExoTheme.radiusSm
        color: root.tone === "primary" && root.enabled ? ExoTheme.accent
            : root.down ? ExoTheme.pressTint(ExoTheme.surfaceRaised)
            : root.hovered || root.activeState ? ExoTheme.surfaceRaised : "transparent"
        border.width: root.visualFocus ? 1 : 0
        border.color: ExoTheme.text
    }
}
