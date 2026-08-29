pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property string label
    required property string valueText
    required property var chips
    required property bool firstRow

    function cellFor(codec: string): var {
        for (let i = 0; i < root.chips.length; ++i) {
            const chip = root.chips[i];
            if (chip.label.indexOf(codec) !== 0)
                continue;
            const match = chip.label.match(/\((\d+)\)/);
            return {
                available: chip.available,
                text: match ? qsTr("Max %1").arg(match[1])
                            : chip.available ? qsTr("Available") : qsTr("Unavailable")
            };
        }
        return { available: false, text: qsTr("Unavailable") };
    }

    implicitHeight: grid.implicitHeight + 2 * ExoTheme.spacingSm

    Rectangle {
        height: 1
        color: ExoTheme.line
        visible: !root.firstRow
        anchors { top: parent.top; right: parent.right; left: parent.left }
    }

    GridLayout {
        id: grid

        columns: 4
        columnSpacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            topMargin: ExoTheme.spacingSm
            bottomMargin: ExoTheme.spacingSm
        }

        Label {
            text: root.label
            wrapMode: Text.WordWrap
            color: ExoTheme.textSecondary
            Layout.preferredWidth: 190
            font { family: ExoTheme.sansFamily; pixelSize: ExoTheme.fontSecondary }
        }

        Label {
            objectName: "capabilityRowValue"
            text: root.valueText
            visible: root.valueText !== ""
            color: ExoTheme.text
            elide: Text.ElideRight
            Layout.columnSpan: 3
            Layout.fillWidth: true
            font { family: ExoTheme.monoFamily; pixelSize: ExoTheme.fontSecondary }
        }

        Repeater {
            model: root.valueText === "" ? ["H.264", "HEVC", "AV1"] : []

            RowLayout {
                required property string modelData
                readonly property var cell: root.cellFor(modelData)
                spacing: ExoTheme.spacingXs
                Layout.fillWidth: true

                ExoGlyph {
                    kind: parent.cell.available ? ExoGlyph.Check : ExoGlyph.Close
                    color: parent.cell.available ? ExoTheme.successText : ExoTheme.textDim
                    Layout.preferredWidth: 12
                    Layout.preferredHeight: 12
                }

                Label {
                    text: parent.cell.text
                    color: parent.cell.available ? ExoTheme.text : ExoTheme.textDim
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    font { family: ExoTheme.monoFamily; pixelSize: ExoTheme.fontCaption }
                }
            }
        }
    }
}
