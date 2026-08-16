import QtQuick
import QtQuick.Controls

// An inline banner that explains a condition the surface it sits on cannot fix
// by itself. `tone` names what the condition MEANS, not what colour to use:
//
//   "info"    — a true statement about the current state; nothing is wrong.
//   "success" — a reassurance: something the user might fear went wrong did not.
//   "warning" — the default; something will not behave as configured.
//   "error"   — the action the user asked for did not happen.
//
// Keeping the default at "warning" means an existing caller that never named a
// tone keeps the banner it already had.
Rectangle {
    id: root

    required property string text
    property string tone: "warning"
    property bool dismissible: false

    signal dismissed()

    readonly property color _accentTone: root.tone === "error" ? ExoTheme.error
                                       : root.tone === "success" ? ExoTheme.success
                                       : root.tone === "info" ? ExoTheme.lineStrong
                                       : ExoTheme.warning
    readonly property color _fill: root.tone === "error" ? ExoTheme.errorSurface
                                 : root.tone === "success" ? Qt.alpha(ExoTheme.success, 0.12)
                                 : root.tone === "info" ? ExoTheme.surfaceRaised
                                 : ExoTheme.warningSurface

    // QCR-508. The tone used to reach the user through the border colour and a
    // ground tint and nothing else. The glyph is the same severity vocabulary
    // the issue cards, the readiness tiles and the Diagnostics verdict band
    // use, and it takes the readable rung because it sits inside the tinted
    // ground (`warning` on `warningSurface` is 2.79:1 in Light).
    readonly property int _glyph: root.tone === "error" ? ExoGlyph.Close
                                : root.tone === "success" ? ExoGlyph.Check
                                : root.tone === "info" ? ExoGlyph.Info
                                : ExoGlyph.Warning
    readonly property color _glyphInk: root.tone === "error" ? ExoTheme.errorText
                                     : root.tone === "success" ? ExoTheme.successText
                                     : root.tone === "info" ? ExoTheme.textSecondary
                                     : ExoTheme.warningText
    // The same fact for a screen reader, which sees neither the glyph nor the
    // colour.
    readonly property string _toneText: root.tone === "error" ? qsTr("Error")
                                      : root.tone === "success" ? qsTr("Success")
                                      : root.tone === "info" ? qsTr("Information")
                                      : qsTr("Warning")

    implicitHeight: Math.max(noticeLabel.implicitHeight + 2 * ExoTheme.spacingSm,
                             dismissButton.visible ? dismissButton.implicitHeight + ExoTheme.spacingSm : 0)
    color: root._fill
    border.width: 1
    border.color: root._accentTone
    radius: ExoTheme.radiusMd

    Accessible.role: Accessible.StaticText
    Accessible.name: root._toneText + ". " + root.text

    ExoGlyph {
        id: toneGlyph

        kind: root._glyph
        color: root._glyphInk
        width: 14
        height: 14
        anchors {
            left: parent.left
            leftMargin: ExoTheme.spacingMd
            top: parent.top
            topMargin: ExoTheme.spacingSm + 2
        }
    }

    Label {
        id: noticeLabel

        text: root.text
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: ExoTheme.text
        anchors {
            fill: parent
            margins: ExoTheme.spacingSm
            leftMargin: ExoTheme.spacingMd + toneGlyph.width + ExoTheme.spacingSm
            rightMargin: dismissButton.visible
                         ? dismissButton.width + 2 * ExoTheme.spacingSm : ExoTheme.spacingMd
        }
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontSecondary
        }
    }

    ExoButton {
        id: dismissButton

        text: qsTr("Dismiss")
        quiet: true
        visible: root.dismissible
        Accessible.name: qsTr("Dismiss notification")
        onClicked: root.dismissed()
        anchors {
            right: parent.right
            verticalCenter: parent.verticalCenter
            rightMargin: ExoTheme.spacingSm
        }
    }
}
