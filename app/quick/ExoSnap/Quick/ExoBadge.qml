import QtQuick
import QtQuick.Controls

// Small tone-tinted pill: diagnostic id chips, the "Elev" lock badge, self-test
// verdicts. Tone keys match diagnostics::IssueToneKey / TileToneKey.
Rectangle {
    id: root

    required property string text
    property string tone: "neutral"
    property bool mono: false

    readonly property color toneColor: root.tone === "blocker" ? ExoTheme.error
                                     : root.tone === "notice" ? ExoTheme.warning
                                     : root.tone === "pass" ? ExoTheme.success
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
        color: root.toneColor
        font {
            family: root.mono ? ExoTheme.monoFamily : ExoTheme.sansFamily
            pixelSize: ExoTheme.fontEyebrow
            weight: Font.DemiBold
        }
    }
}
