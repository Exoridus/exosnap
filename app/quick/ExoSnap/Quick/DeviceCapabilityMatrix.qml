pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Capability matrix for the adapter currently being inspected. Everything shown
// here is resolved by DeviceAdapter: which codecs the GPU advertises, which
// rows are per-adapter probe results and which are system-wide declarations,
// and how the provenance sentence reads.
Rectangle {
    id: root

    required property DeviceAdapter device

    implicitHeight: layout.implicitHeight + 2 * ExoTheme.spacingLg
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusLg

    ColumnLayout {
        id: layout

        spacing: ExoTheme.spacingSm
        anchors {
            fill: parent
            margins: ExoTheme.spacingLg
        }

        RowLayout {
            spacing: ExoTheme.spacingMd
            Layout.fillWidth: true

            Rectangle {
                color: ExoTheme.surfaceRaised
                border.width: 1
                border.color: ExoTheme.accent
                radius: ExoTheme.radiusSm
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                Layout.alignment: Qt.AlignTop

                Rectangle {
                    width: 18
                    height: 18
                    color: ExoTheme.surfaceRaised
                    border.width: 2
                    border.color: ExoTheme.accent
                    radius: 4
                    anchors.centerIn: parent
                }
            }

            ColumnLayout {
                spacing: ExoTheme.spacingXs
                Layout.fillWidth: true

                RowLayout {
                    spacing: ExoTheme.spacingSm
                    Layout.fillWidth: true

                    Label {
                        text: root.device.selectedTitle
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: ExoTheme.text
                        Layout.fillWidth: true
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: 15
                            weight: Font.DemiBold
                        }
                    }

                    Label {
                        text: root.device.selectedKindBadge
                        textFormat: Text.PlainText
                        visible: root.device.selectedKindBadge !== ""
                        color: ExoTheme.textMuted
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: 10
                        }
                    }

                    Rectangle {
                        implicitWidth: stateBadge.implicitWidth + 2 * ExoTheme.spacingSm
                        implicitHeight: 20
                        color: ExoTheme.surfaceRaised
                        border.width: 1
                        border.color: root.device.selectedIsActive ? ExoTheme.success : ExoTheme.line
                        radius: ExoTheme.radiusSm

                        Label {
                            id: stateBadge

                            text: root.device.selectedStateBadge
                            textFormat: Text.PlainText
                            color: root.device.selectedIsActive ? ExoTheme.success : ExoTheme.textMuted
                            anchors.centerIn: parent
                            font {
                                family: ExoTheme.monoFamily
                                pixelSize: 9
                                weight: Font.DemiBold
                            }
                        }
                    }
                }

                Label {
                    text: root.device.selectedSubtitle
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: ExoTheme.textSecondary
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: 12
                    }
                }
            }
        }

        Label {
            text: qsTr("CODEC SUPPORT")
            textFormat: Text.PlainText
            color: ExoTheme.textMuted
            Layout.topMargin: ExoTheme.spacingXs
            Layout.fillWidth: true
            font {
                family: ExoTheme.monoFamily
                pixelSize: 10
                letterSpacing: 1
                weight: Font.DemiBold
            }
        }

        Flow {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            Repeater {
                model: root.device.codecChips

                DeviceCodecChip {
                    required property var modelData

                    label: modelData.label
                    available: modelData.available
                }
            }
        }

        ColumnLayout {
            spacing: 0
            Layout.topMargin: ExoTheme.spacingXs
            Layout.fillWidth: true

            Repeater {
                model: root.device.capabilityRows

                DeviceCapabilityRow {
                    required property int index
                    required property var model

                    firstRow: index === 0
                    label: model.label
                    valueText: model.valueText
                    chips: model.chips
                    Layout.fillWidth: true
                }
            }
        }

        RowLayout {
            spacing: ExoTheme.spacingXs
            Layout.topMargin: ExoTheme.spacingXs
            Layout.fillWidth: true

            Label {
                text: root.device.provenanceOk ? "✓" : "•"
                textFormat: Text.PlainText
                color: root.device.provenanceOk ? ExoTheme.success : ExoTheme.textDim
                Layout.alignment: Qt.AlignTop
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: 11
                }
            }

            Label {
                text: root.device.provenanceText
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: ExoTheme.textMuted
                Layout.fillWidth: true
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 11
                }
            }
        }
    }
}
