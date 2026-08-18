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
                compact: true
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
                    color: root.succeeded ? ExoTheme.successText : ExoTheme.errorText
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontSecondary
                        weight: Font.DemiBold
                    }
                }
            }

            // What was written, on two lines that each elide rather than one run
            // that wrapped anywhere. The file name is the part a reader scans
            // for, so it gets its own line and never breaks; the folder is
            // supporting detail below it. Middle elision keeps both ends — the
            // drive and the extension — which is what makes an elided path
            // readable at all. The full path is one hover away, and the same
            // string is on the clipboard-free route the reveal action takes.
            Label {
                text: root.succeeded ? root.exporter.outputFileName : root.exporter.errorText
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                wrapMode: root.succeeded ? Text.NoWrap : Text.WordWrap
                color: root.succeeded ? ExoTheme.text : ExoTheme.textMuted
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontCaption
                }

                // QCR-509. The full output path was hover-only; the visible
                // run is a middle-elided file name.
                Accessible.role: Accessible.StaticText
                Accessible.name: root.succeeded ? root.exporter.outputFileName : root.exporter.errorText
                Accessible.description: root.succeeded ? root.exporter.outputPath : ""

                HoverHandler {
                    id: resultHover
                }

                ToolTip.visible: root.succeeded && resultHover.hovered
                ToolTip.text: root.exporter.outputPath
            }

            Label {
                text: root.exporter.outputFolder
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                visible: root.succeeded && text.length > 0
                color: ExoTheme.textMuted
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontEyebrow
                }
            }

            // A RowLayout here would report the buttons' natural widths as its
            // own minimum, and a layout honours minimums over the box it was
            // given -- at the 240 px rail that forced every sibling row 32 px
            // past the card's right margin, under the scroll bar. A Flow has no
            // such minimum: it wraps to a second line instead of overflowing.
            Flow {
                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                // One action, not two. "Open folder" and "Show in Explorer" were
                // two borderless text runs for one user task: the second opens
                // the same folder AND selects the file in it, so it does
                // everything the first did. Two labels for one outcome is a
                // choice the user has to make and cannot win.
                ExoButton {
                    text: qsTr("Show in folder")
                    compact: true
                    visible: root.succeeded
                    onClicked: root.exporter.revealFile()
                }

                ExoButton {
                    text: qsTr("Retry")
                    compact: true
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
