pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A reference-page disclosure (spec section 1): ExoDisclosure's chevron header
// on its own card, with a one-line mono summary that stays visible while
// collapsed and an optional trailing action anchored at the header's right
// edge ("Run again", "Rescan", "Create"). Self-test, Hardware capabilities,
// Environment & configuration and Support bundle are four of these, differing
// only in their title, summary and whether they carry an action.
//
// A card each rather than bare rows: the readiness tiles and the issue cards
// above are all framed, and four unframed rows under them read as a footnote
// rather than as the reference the section is.
Rectangle {
    id: root

    property alias title: disclosure.title
    property alias subtitle: disclosure.subtitle
    property alias expanded: disclosure.expanded
    property alias body: disclosure.body
    property string summary: ""
    property Component trailing: null

    implicitHeight: column.implicitHeight + 2 * ExoTheme.spacingSm
    implicitWidth: column.implicitWidth + 2 * ExoTheme.spacingMd
    radius: ExoTheme.radiusMd
    color: ExoTheme.surface
    border {
        width: 1
        color: ExoTheme.line
    }

    ColumnLayout {
        id: column

        anchors {
            fill: parent
            leftMargin: ExoTheme.spacingMd
            rightMargin: ExoTheme.spacingMd
            topMargin: ExoTheme.spacingSm
            bottomMargin: ExoTheme.spacingSm
        }
        spacing: 0

    ExoDisclosure {
        id: disclosure

        Layout.fillWidth: true

        trailing: Component {
            RowLayout {
                spacing: ExoTheme.spacingSm

                Label {
                    text: root.summary
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: ExoTheme.textMuted
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: ExoTheme.fontCaption
                    }
                }

                Loader {
                    sourceComponent: root.trailing
                    Layout.alignment: Qt.AlignVCenter
                }
            }
        }
    }
    }
}
