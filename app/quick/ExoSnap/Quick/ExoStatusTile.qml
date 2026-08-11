import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One readiness-dashboard tile: TITLE · value · sub, tinted by tone, with an
// optional slim usage bar (the Disk tile's fill level) and an optional trailing
// check glyph (the Readiness tile once everything passes).
Rectangle {
    id: root

    required property string title
    required property string value
    property string sub: ""
    property string tone: "neutral"
    property bool showOkGlyph: false
    property bool hasUsageBar: false
    property int usagePercent: 0

    readonly property color toneColor: root.tone === "blocker" ? ExoTheme.error
                                     : root.tone === "notice" ? ExoTheme.warning
                                     : ExoTheme.line

    implicitHeight: column.implicitHeight + 2 * ExoTheme.spacingMd
    implicitWidth: 180
    color: root.tone === "blocker" ? ExoTheme.errorSurface
         : root.tone === "notice" ? ExoTheme.warningSurface
         : ExoTheme.surface
    border.width: 1
    border.color: root.toneColor
    radius: ExoTheme.radiusLg

    Accessible.role: Accessible.StaticText
    Accessible.name: root.title + ": " + root.value + " " + root.sub

    ColumnLayout {
        id: column

        spacing: ExoTheme.spacingXs
        anchors {
            fill: parent
            topMargin: ExoTheme.spacingMd
            bottomMargin: ExoTheme.spacingMd
            leftMargin: ExoTheme.spacingLg
            rightMargin: ExoTheme.spacingLg
        }

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            Label {
                text: root.title.toUpperCase()
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.textMuted
                Layout.fillWidth: true
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 9
                    letterSpacing: 1
                    weight: Font.DemiBold
                }
            }

            Label {
                text: "✓"
                textFormat: Text.PlainText
                visible: root.showOkGlyph
                color: ExoTheme.success
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: 12
                }
            }
        }

        Label {
            text: root.value
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: ExoTheme.text
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: 17
                weight: Font.DemiBold
            }
        }

        Label {
            text: root.sub
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.sub !== ""
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            // A one-line floor: a wrapping sub-line must never clip the tile's own
            // height the way an unconstrained wordWrap label does.
            Layout.minimumHeight: 14
            font {
                family: ExoTheme.sansFamily
                pixelSize: 11
            }
        }

        // Keeps the title/value block pinned to the top when the grid stretches this
        // tile to match a taller sibling, and pushes the usage bar to the bottom edge.
        Item {
            Layout.fillHeight: true
            Layout.minimumHeight: 0
        }

        Rectangle {
            color: ExoTheme.surfaceRaised
            visible: root.hasUsageBar
            radius: 2
            Layout.fillWidth: true
            Layout.preferredHeight: 4
            Layout.topMargin: ExoTheme.spacingXs

            Rectangle {
                width: parent.width * Math.max(0, Math.min(100, root.usagePercent)) / 100
                height: parent.height
                color: ExoTheme.accent
                radius: 2
            }
        }
    }
}
