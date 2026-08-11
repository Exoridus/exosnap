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

    // The capped, centred reading column, shared by the page title and the
    // content below it so both sit on one axis.
    readonly property real contentBox: Math.max(0, root.width - 2 * ExoTheme.pagePadding - ExoTheme.spacingLg)
    readonly property real contentWidth: Math.min(root.contentBox, ExoTheme.contentMaxWidth)
    readonly property real sideInset: Math.max(0, (root.contentBox - root.contentWidth) / 2)

    // Two columns of adapter cards only while each stays wide enough for the
    // title, kind badge and ACTIVE badge to sit on one line. Measured on the
    // capped column, which is the width the cards actually get.
    readonly property bool twoColumn: ExoTheme.isRegular(root.contentWidth)

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
            // Shares the capped column's axis with the content below. Left at
            // full width the page title sat against the window edge while every
            // card under it started 130 px further in, which reads as two pages
            // stacked rather than one.
            Layout.leftMargin: root.sideInset
            Layout.rightMargin: root.sideInset + ExoTheme.spacingLg

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
                width: root.contentWidth
                x: root.sideInset

                Label {
                    text: qsTr("ENCODER DEVICE")
                    textFormat: Text.PlainText
                    color: ExoTheme.textMuted
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: ExoTheme.fontEyebrow
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
                        pixelSize: ExoTheme.fontSecondary
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
                                pixelSize: ExoTheme.fontSecondary
                            }
                        }

                        ExoButton {
                            text: qsTr("Open Settings")
                            Layout.alignment: Qt.AlignVCenter
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
                        pixelSize: ExoTheme.fontEyebrow
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
                                        color: ExoTheme.text
                                        Layout.fillWidth: true
                                        font {
                                            family: ExoTheme.sansFamily
                                            pixelSize: ExoTheme.fontBody
                                            weight: Font.Medium
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
                                            pixelSize: ExoTheme.fontCaption
                                        }
                                    }
                                }

                                // A badge, like every other status this page
                                // states. As bare dim text it read as a fourth
                                // caption in a row that already had two.
                                ExoBadge {
                                    text: qsTr("Planned")
                                    mono: true
                                    Layout.alignment: Qt.AlignVCenter
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
