import QtQuick
import QtQuick.Controls

// Small tone-tinted pill: diagnostic id chips, the "Elev" lock badge, self-test
// verdicts. Tone keys match diagnostics::IssueToneKey / TileToneKey.
Rectangle {
    id: root

    required property string text
    property string tone: "neutral"
    property bool mono: false

    // Two rungs, one rule, used everywhere a tone appears in this frontend: the
    // element that CARRIES the meaning (a word, a severity glyph) takes the
    // readable `*Text` rung; the element that only MARKS it (a border, a dot, a
    // bar) keeps the indicator rung. The badge label is a word — at 10 px, and
    // `warning`/`success` measure 3.94:1 on this fill in Light.
    readonly property color toneColor: root.tone === "blocker" ? ExoTheme.error
                                     : root.tone === "notice" ? ExoTheme.warning
                                     : root.tone === "pass" ? ExoTheme.success
                                     : ExoTheme.textMuted
    readonly property color toneTextColor: root.tone === "blocker" ? ExoTheme.errorText
                                         : root.tone === "notice" ? ExoTheme.warningText
                                         : root.tone === "pass" ? ExoTheme.successText
                                         : ExoTheme.textMuted

    implicitWidth: label.implicitWidth + 2 * ExoTheme.spacingSm
    implicitHeight: 20
    color: ExoTheme.surfaceRaised
    border.width: 1
    border.color: root.toneColor
    radius: ExoTheme.radiusXs

    Accessible.role: Accessible.StaticText
    Accessible.name: root.text

    Label {
        id: label

        anchors.centerIn: parent
        text: root.text
        textFormat: Text.PlainText
        color: root.toneTextColor
        font {
            family: root.mono ? ExoTheme.monoFamily : ExoTheme.sansFamily
            pixelSize: ExoTheme.fontEyebrow
            weight: Font.DemiBold
        }
    }
}
