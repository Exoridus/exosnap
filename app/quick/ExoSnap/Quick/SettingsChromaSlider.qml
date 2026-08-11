import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One 0-100 chroma-key parameter. The adapter converts to and from the
// normalized [0,1] the engine stores, so the percentage never leaks downward.
ExoSettingRow {
    id: root

    required property int value
    required property bool locked

    signal valueEdited(int value)

    RowLayout {
        spacing: ExoTheme.spacingSm
        Layout.fillWidth: true

        ExoSlider {
            from: 0
            to: 100
            stepSize: 1
            value: root.value
            enabled: !root.locked
            Layout.fillWidth: true
            Accessible.name: root.label
            onMovedByUser: value => root.valueEdited(Math.round(value))
        }

        Label {
            text: qsTr("%1 %").arg(root.value)
            textFormat: Text.PlainText
            horizontalAlignment: Text.AlignRight
            color: ExoTheme.textSecondary
            Layout.preferredWidth: 48
            font {
                family: ExoTheme.monoFamily
                pixelSize: 12
            }
        }
    }
}
