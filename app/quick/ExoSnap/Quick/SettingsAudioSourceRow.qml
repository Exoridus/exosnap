import QtQuick
import QtQuick.Layouts

// One audio source: include it, and whether it starts its own track or merges
// onto the track above. Track resolution itself stays engine-side; this row only
// submits the two editable flags.
ExoSettingRow {
    id: root

    required property bool sourceEnabled
    required property bool separateTrack
    required property bool locked
    required property real meterLevel

    signal sourceToggled(bool value)
    signal separateToggled(bool value)

    hint: qsTr("Include this source")
    // Toggle + level meter + merge checkbox share this slot.
    controlWidth: 300

    RowLayout {
        spacing: ExoTheme.spacingSm
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.sourceEnabled
            enabled: !root.locked
            Accessible.name: qsTr("Enable %1").arg(root.label)
            onToggledByUser: value => root.sourceToggled(value)
        }

        ExoLevelMeter {
            level: root.meterLevel
            active: root.sourceEnabled
            Layout.alignment: Qt.AlignVCenter
        }

        ExoCheckBox {
            text: qsTr("Merge with above")
            checked: !root.separateTrack
            enabled: !root.locked && root.sourceEnabled
            Layout.fillWidth: true
            Accessible.name: qsTr("Merge %1 with the track above").arg(root.label)
            onToggledByUser: value => root.separateToggled(!value)
        }
    }
}
