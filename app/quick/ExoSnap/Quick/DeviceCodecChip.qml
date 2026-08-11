import QtQuick
import QtQuick.Controls

// One codec fact: "supported" or "not supported", for exactly one codec. The
// adapter decides whether a chip exists at all — a codec this GPU never
// advertised gets no chip in either state rather than a confusing negative.
Rectangle {
    id: root

    required property string label
    required property bool available

    implicitWidth: chipRow.implicitWidth + 2 * ExoTheme.spacingSm
    implicitHeight: 24
    color: root.available ? ExoTheme.surfaceRaised : ExoTheme.surface
    border.width: 1
    border.color: root.available ? ExoTheme.success : ExoTheme.line
    radius: ExoTheme.radiusSm
    Accessible.role: Accessible.StaticText
    Accessible.name: root.available ? qsTr("%1 supported").arg(root.label)
                                    : qsTr("%1 not supported").arg(root.label)

    Row {
        id: chipRow

        spacing: ExoTheme.spacingXs
        anchors.centerIn: parent

        ExoGlyph {
            kind: root.available ? ExoGlyph.Check : ExoGlyph.Close
            color: root.available ? ExoTheme.success : ExoTheme.textDim
            anchors.verticalCenter: parent.verticalCenter
            width: 11
            height: 11
        }

        Label {
            text: root.label
            textFormat: Text.PlainText
            color: root.available ? ExoTheme.text : ExoTheme.textDim
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
                weight: root.available ? Font.DemiBold : Font.Normal
            }
        }
    }
}
