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

    readonly property color toneColor: root.notRun ? ExoTheme.textDim
                                     : root.tone === "blocker" ? ExoTheme.error
                                     : root.tone === "notice" ? ExoTheme.warning
                                     : ExoTheme.success

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

        Label {
            text: root.notRun ? "—" : root.tone === "pass" ? "✓" : root.tone === "notice" ? "⚠" : "✗"
            textFormat: Text.PlainText
            color: root.toneColor
            Layout.alignment: Qt.AlignTop
            font {
                family: ExoTheme.sansFamily
                pixelSize: 12
            }
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
                pixelSize: 12
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
                pixelSize: 11
            }
        }

        Label {
            text: root.detail
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            Layout.minimumHeight: 14
            Layout.alignment: Qt.AlignTop
            font {
                family: ExoTheme.sansFamily
                pixelSize: 11
            }
        }
    }
}
