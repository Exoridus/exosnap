import QtQuick
import QtQuick.Controls
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
    // The same reading as the meter, in dBFS. Negative infinity means the source
    // produced nothing at all, which is not the same statement as a level at the
    // bottom of the scale and is not printed as one.
    required property real meterDb
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
                id: meter

                level: root.meterLevel
                // Not `sourceEnabled` alone: a receding row (Application audio
                // while a window is not the target) is enabled in settings and
                // contributing nothing, and a live-looking meter there claims
                // otherwise.
                active: root.sourceEnabled && !root.locked
                Layout.fillWidth: true
                Layout.minimumWidth: 72
                Layout.alignment: Qt.AlignVCenter
            }

            // The number the bar is already drawing. The scale IS decibels --
            // the adapter converts a reading into the 0..1 position -- so a bar
            // without it was throwing away the measurement it had, on a card
            // whose next row states a gain in the same unit.
            Label {
                text: !meter.active ? "—"
                    : root.meterDb === Number.NEGATIVE_INFINITY ? qsTr("-∞ dB")
                    : qsTr("%1 dB").arg(root.meterDb.toFixed(1))
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
                color: meter.active ? ExoTheme.textSecondary : ExoTheme.textDim
                Layout.preferredWidth: 58
                Layout.alignment: Qt.AlignVCenter
                Accessible.name: qsTr("%1 level").arg(root.label)
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontSecondary
                }
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
