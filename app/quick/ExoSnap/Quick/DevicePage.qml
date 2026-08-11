pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Device nav area — the encoder-capability home. Lists every DXGI adapter,
// shows the capability matrix for whichever one is being inspected, and names
// the not-yet-wired encoder backends as honest "Planned" rows.
//
// The first adapter scan starts here, the first time the page actually becomes
// visible: construction must stay free of DXGI enumeration and NVENC probing.
Item {
    id: root

    required property DeviceAdapter device

    // Two columns of adapter cards only while each stays wide enough for the
    // title, kind badge and ACTIVE badge to sit on one line.
    readonly property bool twoColumn: ExoTheme.isRegular(root.width)

    signal settingsRequested()

    objectName: "quickDevicePage"

    onVisibleChanged: {
        if (root.visible) {
            root.device.ensureScanned();
        }
    }

    Component.onCompleted: {
        if (root.visible) {
            root.device.ensureScanned();
        }
    }

    ColumnLayout {
        spacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            margins: ExoTheme.pagePadding
        }

        RowLayout {
            spacing: ExoTheme.spacingMd
            Layout.fillWidth: true

            Label {
                text: qsTr("Device")
                textFormat: Text.PlainText
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontPageTitle
                    weight: Font.DemiBold
                }
            }

            ExoButton {
                text: root.device.scanning ? qsTr("Scanning…") : qsTr("Rescan adapters")
                enabled: !root.device.scanning
                Accessible.name: qsTr("Rescan adapters")
                onClicked: root.device.rescan()
            }
        }

        ExoScrollView {
            id: scroll

            contentWidth: availableWidth
            clip: true
            // The vertical scroll bar overlays the content, so the gutter is
            // reserved unconditionally — the page always scrolls.
            rightPadding: ExoTheme.spacingLg
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                spacing: ExoTheme.spacingLg
                // Capped and centred: the capability matrix is a label/value list,
                // and a 1500 px line between the two is harder to read than a
                // shorter one, not richer.
                width: Math.min(scroll.availableWidth, ExoTheme.contentMaxWidth)
                x: Math.max(0, (scroll.availableWidth - width) / 2)

                Label {
                    text: qsTr("ENCODER DEVICE")
                    textFormat: Text.PlainText
                    color: ExoTheme.textMuted
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: 10
                        letterSpacing: 1
                        weight: Font.DemiBold
                    }
                }

                GridLayout {
                    columns: root.twoColumn ? 2 : 1
                    columnSpacing: ExoTheme.spacingMd
                    rowSpacing: ExoTheme.spacingMd
                    visible: root.device.adapterCount > 0
                    Layout.fillWidth: true

                    Repeater {
                        model: root.device.adapters

                        DeviceAdapterCard {
                            required property int index
                            required property var model

                            title: model.title
                            kindBadge: model.kindBadge
                            backendLine: model.backendLine
                            active: model.active
                            inspected: model.selected
                            Layout.fillWidth: true
                            onClicked: root.device.selectAdapter(index)
                        }
                    }
                }

                Label {
                    text: root.device.statusText
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    visible: root.device.statusVisible
                    color: ExoTheme.textMuted
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 12
                    }
                }

                DeviceCapabilityMatrix {
                    device: root.device
                    visible: root.device.matrixVisible
                    Layout.fillWidth: true
                }

                Rectangle {
                    implicitHeight: bannerLayout.implicitHeight + 2 * ExoTheme.spacingSm
                    color: ExoTheme.surfaceRaised
                    border.width: 1
                    border.color: ExoTheme.accent
                    radius: ExoTheme.radiusMd
                    Layout.fillWidth: true

                    RowLayout {
                        id: bannerLayout

                        spacing: ExoTheme.spacingSm
                        anchors {
                            fill: parent
                            margins: ExoTheme.spacingSm
                            leftMargin: ExoTheme.spacingMd
                            rightMargin: ExoTheme.spacingMd
                        }

                        Label {
                            text: root.device.bannerText
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            color: ExoTheme.text
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            font {
                                family: ExoTheme.sansFamily
                                pixelSize: 12
                            }
                        }

                        ExoButton {
                            text: qsTr("Open Settings")
                            quiet: true
                            Layout.alignment: Qt.AlignTop
                            onClicked: root.settingsRequested()
                        }
                    }
                }

                Label {
                    text: qsTr("ENCODER BACKENDS — ROADMAP")
                    textFormat: Text.PlainText
                    color: ExoTheme.textMuted
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: 10
                        letterSpacing: 1
                        weight: Font.DemiBold
                    }
                }

                Rectangle {
                    implicitHeight: roadmapColumn.implicitHeight + 2 * ExoTheme.spacingSm
                    color: ExoTheme.surface
                    border.width: 1
                    border.color: ExoTheme.line
                    radius: ExoTheme.radiusLg
                    Layout.fillWidth: true

                    ColumnLayout {
                        id: roadmapColumn

                        spacing: 0
                        anchors {
                            fill: parent
                            margins: ExoTheme.spacingSm
                            leftMargin: ExoTheme.spacingLg
                            rightMargin: ExoTheme.spacingLg
                        }

                        Repeater {
                            model: root.device.roadmapBackends

                            RowLayout {
                                id: roadmapRow

                                required property int index
                                required property var modelData

                                spacing: ExoTheme.spacingMd
                                Layout.fillWidth: true
                                Layout.topMargin: roadmapRow.index === 0 ? 0 : ExoTheme.spacingSm

                                ColumnLayout {
                                    spacing: 2
                                    Layout.fillWidth: true

                                    Label {
                                        text: roadmapRow.modelData.name
                                        textFormat: Text.PlainText
                                        color: ExoTheme.textSecondary
                                        Layout.fillWidth: true
                                        font {
                                            family: ExoTheme.sansFamily
                                            pixelSize: 12
                                        }
                                    }

                                    Label {
                                        text: roadmapRow.modelData.description
                                        textFormat: Text.PlainText
                                        wrapMode: Text.WordWrap
                                        color: ExoTheme.textMuted
                                        Layout.fillWidth: true
                                        font {
                                            family: ExoTheme.sansFamily
                                            pixelSize: 11
                                        }
                                    }
                                }

                                Label {
                                    text: qsTr("Planned")
                                    textFormat: Text.PlainText
                                    color: ExoTheme.textDim
                                    Layout.alignment: Qt.AlignVCenter
                                    font {
                                        family: ExoTheme.monoFamily
                                        pixelSize: 10
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
