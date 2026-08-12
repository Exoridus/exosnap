pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Label / mono-value fact table — the shared visual language for the Environment
// panel, the Active-configuration reference and the Startup trace.
Rectangle {
    id: root

    required property var rows
    property string labelKey: "label"
    property string valueKey: "value"
    property bool valueRightAligned: false
    property int labelColumnWidth: 180

    implicitHeight: content.implicitHeight + 2 * ExoTheme.spacingSm
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusMd

    ColumnLayout {
        id: content

        spacing: 0
        anchors {
            fill: parent
            margins: ExoTheme.spacingSm
        }

        Repeater {
            model: root.rows

            ColumnLayout {
                id: entry

                required property int index
                required property var modelData

                spacing: 0
                Layout.fillWidth: true

                // No divider above the first row — the same "firstRow" convention the
                // Widgets fact tables use.
                Rectangle {
                    color: ExoTheme.line
                    visible: entry.index > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                }

                RowLayout {
                    spacing: ExoTheme.spacingMd
                    Layout.fillWidth: true
                    Layout.topMargin: ExoTheme.spacingSm
                    Layout.bottomMargin: ExoTheme.spacingSm

                    Label {
                        text: entry.modelData[root.labelKey] ?? ""
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: ExoTheme.textSecondary
                        Layout.preferredWidth: root.labelColumnWidth
                        Layout.fillWidth: root.valueRightAligned
                        Layout.alignment: Qt.AlignTop
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontSecondary
                        }
                    }

                    Label {
                        text: entry.modelData[root.valueKey] ?? ""
                        textFormat: Text.PlainText
                        // Wrap, not WrapAnywhere: a value is diagnostic content
                        // and none of it may be dropped, but a multi-word one
                        // ("MKV + AV1 + Opus + CFR 60 fps", an adapter name and
                        // its driver revision) was being split mid-word when a
                        // space was available two characters earlier. Wrap is
                        // WordWrap with WrapAnywhere as the fallback, so an
                        // unbroken build hash still breaks exactly where it did.
                        wrapMode: Text.Wrap
                        horizontalAlignment: root.valueRightAligned ? Text.AlignRight : Text.AlignLeft
                        color: ExoTheme.text
                        Layout.fillWidth: !root.valueRightAligned
                        Layout.alignment: Qt.AlignTop
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: ExoTheme.fontCaption
                        }
                    }
                }
            }
        }
    }
}
