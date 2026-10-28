pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Popup {
    id: root
    objectName: "audioSourceDetails"
    required property AudioRoutingAdapter routing
    property string sourceId: ""
    property var microphoneDeviceOptions: []
    readonly property var source: routing.sources.find(item => item.id === sourceId) || ({})
    width: Math.min(360, parent.width)
    height: Math.min(560, Overlay.overlay ? Overlay.overlay.height - 2 * ExoTheme.pagePadding : 560)
    padding: ExoTheme.cardPadding
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: ExoTheme.surfaceRaised
        border.color: ExoTheme.lineStrong
        border.width: 1
        radius: ExoTheme.radiusMd
    }
    function patch(key: string, value: var): void {
        const details = {};
        details[key] = value;
        root.routing.setSourceDetails(root.sourceId, details, root.routing.revision);
    }
    contentItem: ScrollView {
        id: scroll
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: scroll.availableWidth
            spacing: ExoTheme.spacingSm
            Label {
                Layout.fillWidth: true
                text: root.source.name || ""
                textFormat: Text.PlainText
                color: ExoTheme.text
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Device and DSP settings apply to this source on every assigned track in the next recording.")
                wrapMode: Text.WordWrap
                color: ExoTheme.textMuted
            }
            Label {
                Layout.fillWidth: true
                visible: root.routing.lastError !== ""
                text: root.routing.lastError
                wrapMode: Text.WordWrap
                color: ExoTheme.error
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }
            ExoSelect {
                Layout.fillWidth: true
                visible: root.source.bindingKind === 1
                enabled: !root.routing.readOnly
                options: root.microphoneDeviceOptions
                value: root.source.followDefault ? "" : root.source.endpointId
                Accessible.name: qsTr("Microphone device")
                onValueActivated: value => root.routing.setSourceDetails(root.sourceId, {
                        endpointId: value,
                        followDefault: value === ""
                    }, root.routing.revision)
            }
            ExoSelect {
                Layout.fillWidth: true
                enabled: !root.routing.readOnly
                options: [
                    {
                        value: 0,
                        label: qsTr("Auto"),
                        selectable: true
                    },
                    {
                        value: 1,
                        label: qsTr("Preserve stereo"),
                        selectable: true
                    },
                    {
                        value: 2,
                        label: qsTr("Mono mix"),
                        selectable: true
                    },
                    {
                        value: 3,
                        label: qsTr("Left channel"),
                        selectable: true
                    },
                    {
                        value: 4,
                        label: qsTr("Right channel"),
                        selectable: true
                    }
                ]
                value: root.source.channelMode
                Accessible.name: qsTr("Channel mode for %1").arg(root.source.name || "")
                onValueActivated: value => root.patch("channelMode", value)
            }
            ExoCheckBox {
                text: qsTr("Mute source on all tracks")
                checked: root.source.muted || false
                enabled: !root.routing.readOnly && !root.routing.recordingActive
                onToggledByUser: value => root.routing.setSourceMute(root.sourceId, value, root.routing.revision)
            }
            Repeater {
                model: [
                    {
                        key: "gainDb",
                        label: qsTr("Source gain in dB")
                    },
                    {
                        key: "hpfCutoffHz",
                        label: qsTr("High-pass cutoff in Hz")
                    },
                    {
                        key: "gateThresholdDb",
                        label: qsTr("Gate threshold in dB")
                    },
                    {
                        key: "agcTargetDb",
                        label: qsTr("Automatic gain target in dB")
                    }
                ]
                delegate: ExoSettingRow {
                    required property var modelData
                    Layout.fillWidth: true
                    label: modelData.label
                    stacked: true
                    ExoTextField {
                        Layout.fillWidth: true
                        enabled: !root.routing.readOnly
                        value: String(root.source[modelData.key] === undefined ? "" : root.source[modelData.key])
                        Accessible.name: modelData.label
                        onCommitted: value => root.patch(modelData.key, Number(value))
                    }
                }
            }
            Repeater {
                model: [
                    {
                        key: "hpfEnabled",
                        label: qsTr("High-pass filter")
                    },
                    {
                        key: "gateEnabled",
                        label: qsTr("Noise gate")
                    },
                    {
                        key: "agcEnabled",
                        label: qsTr("Automatic gain")
                    },
                    {
                        key: "rnnoiseEnabled",
                        label: qsTr("Noise suppression")
                    }
                ]
                delegate: ExoCheckBox {
                    required property var modelData
                    text: modelData.label
                    checked: root.source[modelData.key] || false
                    enabled: !root.routing.readOnly
                    onToggledByUser: value => root.patch(modelData.key, value)
                }
            }
            ExoButton {
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
    }
}
