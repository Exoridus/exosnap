import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Export panel of the Edit rail: an embedded card under the Details card, never
// a modal over the clip it was started from.
//
// Order inside the card is title → status → output rows. The status area is what
// changes, so it sits where the card is anchored; at the 860×700 minimum window
// it stays readable and the output rows are what scrolls out of view instead.
// Nothing above the status moves between states.
//
//   Options --(action bar)--> Running --+-- ok --> Done
//                                       +-- err -> Failed --Retry--> Running
Rectangle {
    id: root

    required property EditExportAdapter exporter

    readonly property bool showsStatus: root.exporter.state !== EditExportAdapter.Options
    readonly property bool succeeded: root.exporter.state === EditExportAdapter.Done

    implicitHeight: layout.implicitHeight + 2 * ExoTheme.spacingLg
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusLg

    ColumnLayout {
        id: layout

        spacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            margins: ExoTheme.spacingLg
        }

        Label {
            text: qsTr("Export")
            textFormat: Text.PlainText
            color: ExoTheme.text
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontBody
                weight: Font.DemiBold
            }
        }

        // ---- Status: running ----
        ColumnLayout {
            spacing: ExoTheme.spacingSm
            visible: root.exporter.running
            Layout.fillWidth: true

            Label {
                text: root.exporter.state === EditExportAdapter.Cancelling ? qsTr("Cancelling…") : qsTr("Exporting…")
                textFormat: Text.PlainText
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                    weight: Font.DemiBold
                }
            }

            ExoProgressBar {
                value: root.exporter.progressPercent / 100
                Layout.fillWidth: true
            }

            ExoButton {
                text: qsTr("Cancel")
                quiet: true
                enabled: root.exporter.state === EditExportAdapter.Running
                Layout.alignment: Qt.AlignRight
                onClicked: root.exporter.cancel()
            }
        }

        // ---- Status: result ----
        ColumnLayout {
            spacing: ExoTheme.spacingSm
            visible: root.showsStatus && !root.exporter.running
            Layout.fillWidth: true

            RowLayout {
                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                ExoBadge {
                    text: root.succeeded ? qsTr("OK") : qsTr("ERR")
                    tone: root.succeeded ? "pass" : "blocker"
                }

                Label {
                    text: root.succeeded ? qsTr("Export complete") : qsTr("Export failed")
                    textFormat: Text.PlainText
                    color: root.succeeded ? ExoTheme.success : ExoTheme.error
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontSecondary
                        weight: Font.DemiBold
                    }
                }
            }

            Label {
                text: root.succeeded ? root.exporter.outputPath : root.exporter.errorText
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: ExoTheme.textMuted
                Layout.fillWidth: true
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontEyebrow
                }
            }

            // A RowLayout here would report the two buttons' natural widths as its
            // own minimum, and a layout honours minimums over the box it was
            // given -- at the 240 px rail that forced every sibling row 32 px
            // past the card's right margin, under the scroll bar. A Flow has no
            // such minimum: it wraps to a second line instead of overflowing.
            Flow {
                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                ExoButton {
                    text: qsTr("Open folder")
                    quiet: true
                    visible: root.succeeded
                    onClicked: root.exporter.openFolder()
                }

                ExoButton {
                    text: qsTr("Show in Explorer")
                    quiet: true
                    visible: root.succeeded
                    onClicked: root.exporter.revealFile()
                }

                ExoButton {
                    text: qsTr("Retry")
                    visible: !root.succeeded
                    enabled: root.exporter.canExport
                    onClicked: root.exporter.retry()
                }
            }
        }

        Rectangle {
            color: ExoTheme.line
            visible: root.showsStatus
            Layout.fillWidth: true
            Layout.preferredHeight: 1
        }

        // ---- Output rows: present in every state, disabled while a run is in
        // flight so the target can never change under a running export. ----
        GridLayout {
            columns: 2
            columnSpacing: ExoTheme.spacingSm
            rowSpacing: ExoTheme.spacingSm
            enabled: !root.exporter.running
            Layout.fillWidth: true

            Label {
                text: qsTr("Container")
                textFormat: Text.PlainText
                color: ExoTheme.textMuted
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }

            ExoSelect {
                objectName: "editExportContainerSelect"
                options: root.exporter.containerOptions
                value: root.exporter.containerKey
                Layout.fillWidth: true
                onValueActivated: value => root.exporter.containerKey = value
            }

            Label {
                text: qsTr("Save")
                textFormat: Text.PlainText
                color: ExoTheme.textMuted
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }

            ExoSelect {
                objectName: "editExportSaveModeSelect"
                options: root.exporter.saveModeOptions
                value: root.exporter.saveModeKey
                Layout.fillWidth: true
                onValueActivated: value => root.exporter.saveModeKey = value
            }
        }

        Label {
            text: root.exporter.destinationText
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }
    }
}
