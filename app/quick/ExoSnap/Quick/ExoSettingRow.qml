import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Label + hint on the left, one control on the right. `stacked` is driven by the
// hosting page rather than this row's own width so the column count can never
// feed back into the layout pass that determines it.
GridLayout {
    id: root

    required property string label
    // A row carries exactly ONE explanation mode:
    //   none      -- label and control, and nothing else to say;
    //   `hint`    -- one visible line that adds to the decision. One line: if the
    //                explanation needs to wrap it is not a hint, it is an `info`;
    //   `info`    -- a focusable trigger opening a paragraph beside the row.
    // Setting both is a design error, not a richer row, and the check below fails
    // loudly rather than rendering two greys that summarise each other.
    property string hint: ""
    property string info: ""
    property string warning: ""
    property bool stacked: false
    property int controlWidth: 220

    Component.onCompleted: {
        if (root.hint !== "" && root.info !== "") {
            console.error("ExoSettingRow '" + root.label
                          + "': hint and info are mutually exclusive -- pick the one-line hint or the popover.");
        }
    }

    default property alias control: controlHost.data

    columns: root.stacked ? 1 : 2
    columnSpacing: ExoTheme.spacingLg
    rowSpacing: ExoTheme.spacingXs

    ColumnLayout {
        spacing: 0
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignVCenter

        RowLayout {
            spacing: ExoTheme.spacingXs
            Layout.fillWidth: true

            // The trigger belongs against the label it explains, so the label only
            // claims the row's width when there is no trigger to sit beside it.
            Label {
                text: root.label
                textFormat: Text.PlainText
                wrapMode: root.info === "" ? Text.WordWrap : Text.NoWrap
                elide: root.info === "" ? Text.ElideNone : Text.ElideRight
                color: ExoTheme.text
                Layout.fillWidth: root.info === ""
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                }
            }

            ExoInfoButton {
                subject: root.label
                body: root.info
                visible: root.info !== ""
                Layout.alignment: Qt.AlignVCenter
            }

            Item {
                Layout.fillWidth: root.info !== ""
            }
        }

        Label {
            text: root.hint
            textFormat: Text.PlainText
            elide: Text.ElideRight
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
            color: ExoTheme.warningText
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
