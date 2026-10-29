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
    property string actionError: ""
    property string errorKey: ""
    onSourceIdChanged: {
        actionError = "";
        errorKey = "";
    }
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
        root.commitDetails(key, details);
    }
    function commitDetails(key: string, details: var): void {
        const accepted = root.routing.setSourceDetails(root.sourceId, details, root.routing.revision);
        root.errorKey = accepted ? "" : key;
        root.actionError = accepted ? "" : root.routing.lastError;
    }
    contentItem: ExoScrollView {
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
                visible: root.actionError !== "" && !["gainDb", "hpfCutoffHz", "gateThresholdDb", "agcTargetDb"].includes(root.errorKey)
                text: root.actionError
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
                onValueActivated: value => root.commitDetails("device", {
                        endpointId: value,
                        followDefault: value === ""
                    })
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
                onToggledByUser: value => {
                    const accepted = root.routing.setSourceMute(root.sourceId, value, root.routing.revision);
                    root.errorKey = accepted ? "" : "mute";
                    root.actionError = accepted ? "" : root.routing.lastError;
                }
            }
            Repeater {
                model: [
                    {
                        key: "gainDb",
                        label: qsTr("Source gain"),
                        minimum: root.routing.minimumGainDb,
                        maximum: root.routing.maximumGainDb,
                        step: 0.5,
                        unit: qsTr("dB")
                    },
                    {
                        key: "hpfCutoffHz",
                        label: qsTr("High-pass cutoff"),
                        minimum: 20,
                        maximum: 1000,
                        step: 10,
                        unit: qsTr("Hz")
                    },
                    {
                        key: "gateThresholdDb",
                        label: qsTr("Gate threshold"),
                        minimum: -80,
                        maximum: 0,
                        step: 1,
                        unit: qsTr("dB")
                    },
                    {
                        key: "agcTargetDb",
                        label: qsTr("Automatic gain target"),
                        minimum: -40,
                        maximum: 0,
                        step: 1,
                        unit: qsTr("dB")
                    }
                ]
                delegate: ExoSettingRow {
                    id: parameterRow
                    required property var modelData
                    readonly property real storedValue: root.source[parameterRow.modelData.key] === undefined ? root.routing.sourceDetailDefaults[parameterRow.modelData.key] : root.source[parameterRow.modelData.key]
                    readonly property bool withinRange: Number.isFinite(parameterRow.storedValue) && parameterRow.storedValue >= parameterRow.modelData.minimum && parameterRow.storedValue <= parameterRow.modelData.maximum
                    Layout.fillWidth: true
                    label: parameterRow.modelData.label
                    stacked: true
                    ExoNumberField {
                        objectName: "audioSourceParameter-" + parameterRow.modelData.key
                        Layout.fillWidth: true
                        visible: parameterRow.withinRange
                        enabled: !root.routing.readOnly
                        from: parameterRow.modelData.minimum
                        to: parameterRow.modelData.maximum
                        decimals: 3
                        stepSize: parameterRow.modelData.step
                        suffix: parameterRow.modelData.unit
                        value: parameterRow.withinRange ? storedValue : root.routing.sourceDetailDefaults[parameterRow.modelData.key]
                        errorText: root.errorKey === parameterRow.modelData.key ? root.actionError : ""
                        resetEnabled: true
                        Accessible.name: parameterRow.modelData.label
                        onValueCommitted: value => root.patch(parameterRow.modelData.key, value)
                        onResetRequested: root.patch(parameterRow.modelData.key, root.routing.sourceDetailDefaults[parameterRow.modelData.key])
                    }
                    ExoTextField {
                        objectName: "audioSourceRetainedParameter-" + parameterRow.modelData.key
                        Layout.fillWidth: true
                        visible: !parameterRow.withinRange
                        enabled: !root.routing.readOnly
                        readOnly: true
                        errorText: root.errorKey === parameterRow.modelData.key ? root.actionError : ""
                        value: qsTr("%1 %2").arg(parameterRow.storedValue === Number.NEGATIVE_INFINITY ? "−∞" : Number(parameterRow.storedValue).toLocaleString(locale, 'f', 3)).arg(parameterRow.modelData.unit)
                        trailingGlyph: ExoGlyph.Refresh
                        trailingAccessibleName: qsTr("Reset to default")
                        Accessible.name: parameterRow.modelData.label
                        Accessible.description: hasError ? errorText : qsTr("Stored value is outside the editable range. Reset to default to edit it.")
                        ToolTip.visible: hovered || activeFocus
                        ToolTip.text: Accessible.description
                        onTrailingClicked: root.patch(parameterRow.modelData.key, root.routing.sourceDetailDefaults[parameterRow.modelData.key])
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
