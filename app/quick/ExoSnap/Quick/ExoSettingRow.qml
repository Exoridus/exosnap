import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Label + hint on the left, one control on the right. `stacked` is driven by the
// hosting page rather than this row's own width so the column count can never
// feed back into the layout pass that determines it.
GridLayout {
    id: root

    required property string label
    property string hint: ""
    property string warning: ""
    property bool stacked: false
    property int controlWidth: 220

    default property alias control: controlHost.data

    columns: root.stacked ? 1 : 2
    columnSpacing: ExoTheme.spacingLg
    rowSpacing: ExoTheme.spacingXs

    ColumnLayout {
        spacing: 0
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignVCenter

        Label {
            text: root.label
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.text
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontBody
            }
        }

        Label {
            text: root.hint
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.hint !== ""
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }

        Label {
            text: root.warning
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.warning !== ""
            color: ExoTheme.warning
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }
    }

    ColumnLayout {
        id: controlHost

        spacing: ExoTheme.spacingXs
        Layout.fillWidth: root.stacked
        Layout.preferredWidth: root.stacked ? -1 : root.controlWidth
        Layout.alignment: root.stacked ? Qt.AlignLeft : Qt.AlignRight | Qt.AlignVCenter
    }
}
