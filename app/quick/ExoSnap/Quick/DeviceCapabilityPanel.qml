pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Detected adapters and the encoder capabilities of whichever one is being
// inspected — the whole of what the former top-level Device page owned, minus
// its roadmap band.
//
// It lives on Diagnostics because every value here is something ExoSnap
// MEASURES about this machine: which GPUs exist, which one is actually carrying
// the encode, and what that silicon advertises. None of it is a choice the user
// makes, so none of it belongs in Settings.
//
// The first adapter scan starts from here, the first time the panel is really
// on screen: DXGI enumeration and an NVENC probe are seconds of work on a cold
// driver, and a collapsed section must not pay for them.
ColumnLayout {
    id: root

    required property DeviceAdapter device

    // Two columns of adapter cards only while each stays wide enough for the
    // title, kind badge and ACTIVE badge to sit on one line.
    readonly property bool twoColumn: ExoTheme.isRegular(root.width)

    objectName: "quickDeviceCapabilityPanel"

    spacing: ExoTheme.spacingMd

    onVisibleChanged: {
        if (root.visible)
            root.device.ensureScanned();
    }

    Component.onCompleted: {
        if (root.visible)
            root.device.ensureScanned();
    }

    RowLayout {
        spacing: ExoTheme.spacingMd
        Layout.fillWidth: true

        Label {
            text: root.device.bannerText
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            Layout.minimumHeight: 17
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        ExoButton {
            text: root.device.scanning ? qsTr("Scanning…") : qsTr("Rescan")
            quiet: true
            enabled: !root.device.scanning
            Accessible.name: qsTr("Rescan adapters")
            Layout.alignment: Qt.AlignTop
            onClicked: root.device.rescan()
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
}
