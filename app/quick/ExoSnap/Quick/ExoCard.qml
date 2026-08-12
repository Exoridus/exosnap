import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property string title
    property string subtitle: ""

    default property alias content: contentColumn.data

    implicitHeight: layout.implicitHeight + 2 * ExoTheme.cardPadding
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusLg

    ColumnLayout {
        id: layout

        // The heading binds to its own rows, so the gap under it is smaller than
        // the gap between rows. Density on this page decides how much of the
        // recording configuration is visible without scrolling.
        spacing: ExoTheme.spacingSm
        anchors {
            fill: parent
            margins: ExoTheme.cardPadding
        }

        Label {
            text: root.title
            textFormat: Text.PlainText
            color: ExoTheme.text
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSectionTitle
                weight: Font.DemiBold
            }
        }

        Label {
            text: root.subtitle
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.subtitle !== ""
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            Layout.topMargin: -ExoTheme.spacingXs - 1
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        ColumnLayout {
            id: contentColumn

            spacing: ExoTheme.rowSpacing
            Layout.fillWidth: true
            Layout.topMargin: ExoTheme.spacingXs
        }
    }
}
