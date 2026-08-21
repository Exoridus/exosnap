import QtQuick
import QtQuick.Layouts

// One audio source: include it, and whether it starts its own track or mixes
// onto the track above. Track resolution itself stays engine-side; this row only
// submits the two editable flags.
ExoSettingRow {
    id: root

    required property bool sourceEnabled
    required property bool separateTrack
    required property bool locked
    required property real meterLevel
    // False only for a source row that renders as the topmost VISIBLE row:
    // "mix into previous track" is meaningless when there is no visible row above it.
    property bool showMixOption: true

    signal sourceToggled(bool value)
    signal separateToggled(bool value)

    // No default hint: "Include this source" restated the switch beside it on
    // every row. The one row that owes an explanation (Application audio, while
    // a window is not the capture target) passes its own.
    // Toggle + level meter + mix checkbox share this slot. Set against both
    // ends: below ~320 the row carrying the mix checkbox squeezes the meter
    // back into a stub, and above it the label column loses the Application
    // audio row's explanatory line to elision.
    controlWidth: 320

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

        // The meter absorbs the row's leftover width, which is also what keeps the
        // checkbox after it flush against the card's right edge. It used to be a
        // fixed 80 px with an empty spacer doing that job, and the result read as
        // a stub with a hole beside it rather than as a level indicator.
        ExoLevelMeter {
            level: root.meterLevel
            // Not `sourceEnabled` alone: a receding row (Application audio while
            // a window is not the target) is enabled in settings and contributing
            // nothing, and a live-looking meter there claims otherwise.
            active: root.sourceEnabled && !root.locked
            Layout.fillWidth: true
            Layout.minimumWidth: 72
            // Capped, or the row without a mix checkbox stretches the bar across
            // the whole card and it stops reading as a meter beside its switch.
            Layout.maximumWidth: 180
            Layout.alignment: Qt.AlignVCenter
        }

        // Takes the width the capped meter leaves, so the checkbox stays flush
        // against the card's right edge in both row shapes.
        Item {
            Layout.fillWidth: true
        }

        ExoCheckBox {
            text: qsTr("Mix into previous track")
            checked: !root.separateTrack
            enabled: !root.locked && root.sourceEnabled
            visible: root.showMixOption
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            Accessible.name: qsTr("Mix %1 into the previous track").arg(root.label)
            onToggledByUser: value => root.separateToggled(!value)
        }
    }
}
