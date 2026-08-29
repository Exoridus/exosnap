import QtQuick
import QtQuick.Layouts

// One audio source: include it, and whether it starts its own track or mixes
// onto the track above. Track resolution itself stays engine-side; this row only
// submits the two editable flags.
//
// The mix option is a line of its own under the row rather than a third control
// in the row's slot. Inside the slot it had to reserve its width in EVERY row,
// including the ones that cannot offer it, or the level meter beside it would
// change length depending on a neighbouring row's state.
ColumnLayout {
    id: root

    required property string label
    required property bool sourceEnabled
    required property bool separateTrack
    required property bool locked
    required property real meterLevel
    required property bool stacked
    property string hint: ""
    // False only for a source row that renders as the topmost VISIBLE row:
    // "mix into previous track" is meaningless when there is no visible row above it.
    property bool showMixOption: true

    signal sourceToggled(bool value)
    signal separateToggled(bool value)

    spacing: ExoTheme.spacingXs

    ExoSettingRow {
        // No hint by default: "Include this source" restated the switch beside it
        // on every row. The one row that owes an explanation (Application audio,
        // while a window is not the capture target) passes its own.
        label: root.label
        hint: root.hint
        stacked: root.stacked
        // Switch and level meter share the value side, so this row takes the wide
        // slot: narrower and the meter squeezes back into a stub.
        controlWidth: ExoTheme.controlSlotWide
        Layout.fillWidth: true

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            ExoSwitch {
                checked: root.sourceEnabled
                enabled: !root.locked
                Accessible.name: qsTr("Enable %1").arg(root.label)
                Layout.alignment: Qt.AlignVCenter
                onToggledByUser: value => root.sourceToggled(value)
            }

            ExoLevelMeter {
                level: root.meterLevel
                // Not `sourceEnabled` alone: a receding row (Application audio
                // while a window is not the target) is enabled in settings and
                // contributing nothing, and a live-looking meter there claims
                // otherwise.
                active: root.sourceEnabled && !root.locked
                Layout.fillWidth: true
                Layout.minimumWidth: 72
                // Capped, or in the stacked layout the bar stretches across the
                // whole card and stops reading as a meter beside its switch.
                Layout.maximumWidth: 220
                Layout.alignment: Qt.AlignVCenter
            }
        }
    }

    ExoCheckBox {
        id: mixOption

        text: qsTr("Mix into previous track")
        checked: !root.separateTrack
        enabled: !root.locked && root.sourceEnabled
        visible: root.showMixOption
        // Indented under the label it modifies: it is a qualifier on this source,
        // not a setting of its own rank.
        Layout.leftMargin: ExoTheme.spacingLg
        Layout.alignment: Qt.AlignLeft
        Accessible.name: qsTr("Mix %1 into the previous track").arg(root.label)
        onToggledByUser: value => root.separateToggled(!value)
    }
}
