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
    // A second qualifier line, below `sub`. Empty for the readiness tiles, which
    // have one fact to add; the live tiles use it for the thing that is missing
    // when the measurement above it is unavailable (why present diagnostics are
    // off, why a remaining time cannot be estimated).
    property string detail: ""
    property string tone: "neutral"
    property bool showOkGlyph: false
    property bool hasUsageBar: false
    property int usagePercent: 0

    readonly property color toneColor: root.tone === "blocker" ? ExoTheme.error
                                     : root.tone === "notice" ? ExoTheme.warning
                                     : ExoTheme.line

    // QCR-507. The tile used to say its severity with colour alone: a blocker
    // and a notice differed by the hue of a 1 px border and a background tint,
    // which is nothing to a user with a colour-vision deficiency and very
    // little on a glanced-at dashboard. The glyph below is the same vocabulary
    // the issue cards and the Diagnostics verdict band already use — ✕ for a
    // blocker, ⚠ for a notice, ✓ for a tile that is clear — so this is one
    // severity language across the product, not a second one.
    readonly property int toneGlyph: root.tone === "blocker" ? ExoGlyph.Close
                                   : root.tone === "notice" ? ExoGlyph.Warning
                                   : root.showOkGlyph ? ExoGlyph.Check
                                   : ExoGlyph.Invalid
    readonly property color toneGlyphColor: root.tone === "blocker" ? ExoTheme.errorText
                                          : root.tone === "notice" ? ExoTheme.warningText
                                          : ExoTheme.successText
    // Said in words for a screen reader, which cannot see either cue.
    readonly property string severityText: root.tone === "blocker" ? qsTr("Blocked")
                                         : root.tone === "notice" ? qsTr("Caution")
                                         : root.showOkGlyph ? qsTr("Ready") : ""

    implicitHeight: column.implicitHeight + 2 * ExoTheme.spacingLg
    implicitWidth: 210
    color: root.tone === "blocker" ? ExoTheme.errorSurface
         : root.tone === "notice" ? ExoTheme.warningSurface
         : ExoTheme.surface
    border.width: 1
    border.color: root.toneColor
    radius: ExoTheme.radiusLg

    Accessible.role: Accessible.StaticText
    readonly property string spokenBody: root.detail === ""
                                       ? root.title + ": " + root.value + " " + root.sub
                                       : root.title + ": " + root.value + " " + root.sub + ". " + root.detail
    Accessible.name: root.severityText === "" ? root.spokenBody : root.severityText + ". " + root.spokenBody

    ColumnLayout {
        id: column

        spacing: ExoTheme.spacingXs
        anchors {
            fill: parent
            topMargin: ExoTheme.spacingLg
            bottomMargin: ExoTheme.spacingLg
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
                    pixelSize: ExoTheme.fontEyebrow
                    letterSpacing: 1
                    weight: Font.DemiBold
                }
            }

            ExoGlyph {
                kind: root.toneGlyph
                visible: root.toneGlyph !== ExoGlyph.Invalid
                color: root.toneGlyphColor
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
            }
        }

        // The measurement the tile exists to report. On the shared value rung, so
        // it is unmistakably more important than its own caption above it and its
        // qualifier below — at 17 px against a 9 px caption and an 11 px sub it
        // was just the largest of three small things.
        Label {
            text: root.value
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: ExoTheme.text
            Layout.fillWidth: true
            Layout.topMargin: ExoTheme.spacingXs
            // A long value shrinks rather than truncating. "NVIDIA GeForce RTX
            // 5070 Ti" does not fit a tile at the value size, and the answer is
            // not to show the user "NVIDIA GeForce RTX …" — the whole point of
            // the encoder tile is which encoder. Short values keep the full rung;
            // elide stays as the floor for a value no size can fit.
            fontSizeMode: Text.HorizontalFit
            minimumPixelSize: ExoTheme.fontSectionTitle
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontValue
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
            Layout.minimumHeight: 16
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }

        Label {
            text: root.detail
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.detail !== ""
            color: ExoTheme.textDim
            Layout.fillWidth: true
            // Same one-line floor as `sub`: an unconstrained wrapping label
            // clips its own tile at the 860 px width class.
            Layout.minimumHeight: 16
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
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
