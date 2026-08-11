import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "Details" card of the Edit rail: the clip's seven facts, mono values on the
// right. No fact is ever dropped — the narrow breakpoint tightens the same
// content and hands the difference to the export panel below it.
Rectangle {
    id: root

    required property EditSessionAdapter session
    property bool compact: false

    implicitHeight: layout.implicitHeight + 2 * (root.compact ? ExoTheme.spacingSm : ExoTheme.spacingLg)
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusLg

    ColumnLayout {
        id: layout

        spacing: root.compact ? ExoTheme.spacingXs : ExoTheme.spacingSm
        anchors {
            fill: parent
            leftMargin: ExoTheme.spacingMd
            rightMargin: ExoTheme.spacingMd
            topMargin: root.compact ? ExoTheme.spacingSm : ExoTheme.spacingLg
            bottomMargin: root.compact ? ExoTheme.spacingSm : ExoTheme.spacingLg
        }

        Label {
            text: qsTr("Details")
            textFormat: Text.PlainText
            color: ExoTheme.text
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: 13
                weight: Font.DemiBold
            }
        }

        // The shared fact table, with its own card chrome dropped: it already
        // sits inside one, and a box in a box reads as two panels.
        ExoKeyValueTable {
            rows: root.session.facts
            valueRightAligned: true
            labelColumnWidth: root.compact ? 68 : 88
            color: ExoTheme.surface
            border.width: 0
            radius: 0
            Layout.fillWidth: true
        }
    }
}
