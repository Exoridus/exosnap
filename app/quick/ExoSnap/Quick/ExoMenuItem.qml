import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// One row of an ExoMenu. A row that opens a submenu shows the submenu's
// `valueText` and an arrow, so a selection reads as "Label  Value >".
MenuItem {
    id: root

    // Marks the active choice of a selection submenu.
    property bool current: false
    readonly property string valueText: (root.subMenu as ExoMenu)?.valueText ?? ""

    implicitWidth: Math.max(160, contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: ExoTheme.controlHeight
    leftPadding: ExoTheme.spacingMd
    rightPadding: ExoTheme.spacingLg
    topPadding: 0
    bottomPadding: 0

    contentItem: RowLayout {
        spacing: ExoTheme.spacingSm

        Label {
            Layout.fillWidth: true
            verticalAlignment: Text.AlignVCenter
            text: root.text
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: root.enabled ? ExoTheme.text : ExoTheme.textDim
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontBody
            }
        }

        Label {
            visible: root.valueText.length > 0
            Layout.maximumWidth: 180
            verticalAlignment: Text.AlignVCenter
            text: root.valueText
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: root.enabled ? ExoTheme.textMuted : ExoTheme.textDim
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontBody
            }
        }

        ExoGlyph {
            visible: root.subMenu !== null || root.current
            kind: root.subMenu !== null ? ExoGlyph.ArrowRight : ExoGlyph.Check
            color: root.current ? ExoTheme.accent : ExoTheme.textMuted
            Layout.preferredWidth: 12
            Layout.preferredHeight: 12
        }
    }

    background: Rectangle {
        color: root.down ? ExoTheme.surfaceHover
             : root.highlighted && root.enabled ? ExoTheme.hoverTint(ExoTheme.surfaceRaised) : "transparent"
        radius: ExoTheme.radiusSm
    }

    // The Basic style draws a tick column for checkable items; nothing in this
    // product uses one, and an empty column just indents every label.
    indicator: Item {}
    arrow: Item {}
}
