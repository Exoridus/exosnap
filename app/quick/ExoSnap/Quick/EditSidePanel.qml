import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A pinnable side panel of the Edit page: a titled header with the pin and
// close actions as icon buttons, and the panel's own content below it.
Rectangle {
    id: root

    required property string title
    required property bool pinned
    // Pinning narrows the preview; the page disables it when the preview would
    // drop below its usable width.
    property bool canPin: true
    property string pinObjectName: ""
    property string closeText: qsTr("Close")
    default property alias content: body.data

    signal pinToggled()
    signal closeRequested()

    color: ExoTheme.surface
    radius: ExoTheme.radiusMd
    border.color: ExoTheme.lineStrong

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: ExoTheme.spacingMd
        spacing: ExoTheme.spacingMd

        RowLayout {
            Layout.fillWidth: true
            spacing: ExoTheme.spacingXs

            Label {
                Layout.fillWidth: true
                text: root.title
                elide: Text.ElideRight
                color: ExoTheme.text
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontSectionTitle
                font.weight: Font.DemiBold
                Accessible.role: Accessible.Heading
            }
            EditActionButton {
                objectName: root.pinObjectName
                text: root.pinned ? qsTr("Unpin") : qsTr("Pin")
                glyph: ExoGlyph.Pin
                enabled: root.pinned || root.canPin
                checkable: true
                checked: root.pinned
                onClicked: {
                    root.pinToggled();
                    // A click toggles `checked` itself; the page may also unpin on
                    // its own, so the state must keep following `pinned`.
                    checked = Qt.binding(() => root.pinned);
                }
            }
            EditActionButton {
                text: root.closeText
                glyph: ExoGlyph.Close
                onClicked: root.closeRequested()
            }
        }

        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
