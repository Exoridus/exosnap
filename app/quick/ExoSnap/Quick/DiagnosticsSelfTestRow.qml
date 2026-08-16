import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One self-test result. "Not run" is a typed fact from the extraction, never a
// substring sniff on the detail text, so the row renders it as its own state
// rather than dressing a failure up as one.
Rectangle {
    id: root

    required property string title
    required property string statusText
    required property string detail
    property string tone: "pass"
    property bool notRun: false

    // Everything the tone touches here is content — the em dash, the severity
    // glyph and the status word — so all of it takes the readable rung.
    readonly property color toneColor: root.notRun ? ExoTheme.textDim
                                     : root.tone === "blocker" ? ExoTheme.errorText
                                     : root.tone === "notice" ? ExoTheme.warningText
                                     : ExoTheme.successText

    implicitHeight: row.implicitHeight + 2 * ExoTheme.spacingSm
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusSm

    Accessible.role: Accessible.StaticText
    Accessible.name: root.title + ": " + root.statusText

    RowLayout {
        id: row

        spacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            topMargin: ExoTheme.spacingSm
            bottomMargin: ExoTheme.spacingSm
            leftMargin: ExoTheme.spacingMd
            rightMargin: ExoTheme.spacingMd
        }

        // "Not run" stays an em dash: that is the app's convention for a value
        // that has no measurement, not an icon.
        Label {
            text: "—"
            textFormat: Text.PlainText
            visible: root.notRun
            color: root.toneColor
            Layout.alignment: Qt.AlignTop
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        ExoGlyph {
            kind: root.tone === "pass" ? ExoGlyph.Check
                  : root.tone === "notice" ? ExoGlyph.Warning : ExoGlyph.Close
            visible: !root.notRun
            color: root.toneColor
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 13
            Layout.preferredHeight: 13
        }

        Label {
            text: root.title
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.text
            Layout.preferredWidth: 170
            Layout.alignment: Qt.AlignTop
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        Label {
            text: root.statusText
            textFormat: Text.PlainText
            color: root.toneColor
            Layout.preferredWidth: 56
            Layout.alignment: Qt.AlignTop
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontCaption
            }
        }

        Label {
            text: root.detail
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            Layout.minimumHeight: 16
            Layout.alignment: Qt.AlignTop
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }
    }
}
