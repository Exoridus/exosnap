pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A reference-page disclosure (spec section 1): ExoDisclosure's chevron header,
// with a one-line mono summary that stays visible while collapsed and an
// optional trailing action anchored at the header's right edge ("Run again",
// "Rescan", "Create"). Self-test, Hardware capabilities, Environment &
// configuration and Support bundle are four of these, differing only in their
// title, summary and whether they carry an action.
ColumnLayout {
    id: root

    property alias title: disclosure.title
    property alias subtitle: disclosure.subtitle
    property alias expanded: disclosure.expanded
    property alias body: disclosure.body
    property string summary: ""
    property Component trailing: null

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
