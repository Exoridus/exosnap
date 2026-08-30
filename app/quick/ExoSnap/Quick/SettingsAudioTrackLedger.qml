import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What the source rows above actually resolve to. The engine owns track
// resolution and hands back a plan; this reads that plan back, so the card
// cannot claim an arrangement the file will not have.
//
// It is the only place the mix checkboxes become visible as an outcome. Without
// it, "mix into previous track" is a flag whose effect is only discoverable by
// recording something and opening the result.
ColumnLayout {
    id: root

    required property var tracks

    spacing: ExoTheme.spacingSm

    Rectangle {
        color: ExoTheme.line
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        Layout.topMargin: ExoTheme.spacingXs
    }

    Flow {
        spacing: ExoTheme.spacingSm
        Layout.fillWidth: true

        Label {
            text: qsTr("Result")
            textFormat: Text.PlainText
            height: chipRow.height
            verticalAlignment: Text.AlignVCenter
            color: ExoTheme.textMuted
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontEyebrow
                letterSpacing: 1.2
                capitalization: Font.AllUppercase
            }
        }

        Row {
            id: chipRow

            spacing: ExoTheme.spacingSm

            Repeater {
                model: root.tracks

                Rectangle {
                    id: chip

                    required property var modelData

                    color: ExoTheme.surfaceRaised
                    border.width: 1
                    border.color: ExoTheme.line
                    radius: ExoTheme.radiusPill
                    implicitWidth: chipContent.implicitWidth + 2 * ExoTheme.spacingMd
                    implicitHeight: chipContent.implicitHeight + ExoTheme.spacingSm

                    RowLayout {
                        id: chipContent

                        anchors.centerIn: parent
                        spacing: ExoTheme.spacingSm

                        Label {
                            text: chip.modelData.track
                            textFormat: Text.PlainText
                            color: ExoTheme.textMuted
                            font {
                                family: ExoTheme.monoFamily
                                pixelSize: ExoTheme.fontCaption
                            }
                        }

                        Label {
                            text: chip.modelData.label
                            textFormat: Text.PlainText
                            color: ExoTheme.textSecondary
                            font {
                                family: ExoTheme.sansFamily
                                pixelSize: ExoTheme.fontCaption
                            }
                        }
                    }
                }
            }
        }

        // An empty plan is a real state, not a missing one: every source is off
        // and the recording will have no audio at all.
        Label {
            text: qsTr("No audio in the recording")
            textFormat: Text.PlainText
            visible: root.tracks.length === 0
            height: chipRow.height
            verticalAlignment: Text.AlignVCenter
            color: ExoTheme.textDim
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }
    }
}
