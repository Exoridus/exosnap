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

    // The default valueFromText expects a plain number and fails on the
    // suffix textFromValue appended above. SpinBox re-parses the displayed
    // text through this on every revalidation point, including focus loss,
    // so without a matching override a suffixed field snaps to `from` the
    // moment it is left. Falling back to the current value (not `from`) on a
    // genuinely unparseable string, since a value field should never jump to
    // its floor because of a formatting artifact it introduced itself.
    valueFromText: (text, locale) => {
        const digitsOnly = text.replace(/[^0-9\u2212-]/g, '').replace(/\u2212/g, '-');
        if (digitsOnly === '' || digitsOnly === '-')
            return root.value;
        const parsed = Number.fromLocaleString(locale, digitsOnly);
        return Number.isFinite(parsed) ? parsed : root.value;
    }

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
            pixelSize: ExoTheme.fontBody
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

        ExoGlyph {
            kind: ExoGlyph.Plus
            anchors.centerIn: parent
            color: root.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
            width: 12
            height: 12
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

        ExoGlyph {
            kind: ExoGlyph.Minus
            anchors.centerIn: parent
            color: root.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
            width: 12
            height: 12
        }
    }

    onValueModified: root.valueCommitted(root.value)
}
