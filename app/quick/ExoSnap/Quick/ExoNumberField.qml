import QtQuick
import QtQuick.Controls.Basic

SpinBox {
    id: root

    property string suffix: ""

    signal valueCommitted(int value)

    implicitHeight: ExoTheme.controlHeight
    editable: true
    focusPolicy: Qt.StrongFocus

    textFromValue: (value, locale) => root.suffix === ""
                                      ? Number(value).toLocaleString(locale, 'f', 0)
                                      : qsTr("%1 %2").arg(Number(value).toLocaleString(locale, 'f', 0)).arg(root.suffix)

    contentItem: TextInput {
        text: root.displayText
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignLeft
        readOnly: !root.editable
        validator: root.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        selectByMouse: true
        color: root.enabled ? ExoTheme.text : ExoTheme.textDim
        selectionColor: ExoTheme.accent
        selectedTextColor: ExoTheme.accentInk
        leftPadding: ExoTheme.spacingMd
        font {
            family: ExoTheme.sansFamily
            pixelSize: 13
        }
    }

    background: Rectangle {
        color: root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: 1
        border.color: root.activeFocus ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
    }

    up.indicator: Item {
        x: root.width - width
        height: parent.height / 2
        implicitWidth: 26

        Rectangle {
            color: ExoTheme.surfaceHover
            visible: root.up.pressed
            anchors.fill: parent
        }

        Label {
            text: "+"
            textFormat: Text.PlainText
            anchors.centerIn: parent
            color: root.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
            font.pixelSize: 13
        }
    }

    down.indicator: Item {
        x: root.width - width
        y: parent.height / 2
        height: parent.height / 2
        implicitWidth: 26

        Rectangle {
            color: ExoTheme.surfaceHover
            visible: root.down.pressed
            anchors.fill: parent
        }

        Label {
            text: "−"
            textFormat: Text.PlainText
            anchors.centerIn: parent
            color: root.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
            font.pixelSize: 13
        }
    }

    onValueModified: root.valueCommitted(root.value)
}
