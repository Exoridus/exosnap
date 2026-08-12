import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Rule header for an Expert taxonomy section: kicker, optional meta, hairline.
RowLayout {
    id: root

    required property string title
    property string meta: ""

    spacing: ExoTheme.spacingSm

    Label {
        text: root.title
        textFormat: Text.PlainText
        color: ExoTheme.textMuted
        font {
            family: ExoTheme.monoFamily
            pixelSize: ExoTheme.fontEyebrow
            letterSpacing: 1
            weight: Font.DemiBold
        }
    }

    Rectangle {
        color: ExoTheme.line
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        Layout.alignment: Qt.AlignVCenter
    }

    Label {
        text: root.meta
        textFormat: Text.PlainText
        visible: root.meta !== ""
        color: ExoTheme.textDim
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontCaption
        }
    }
}
