import QtQuick
import QtQuick.Controls.Basic

SpinBox {
    id: root

    property string suffix: ""

    // Both steppers are stacked on the right edge, so they occupy ONE column of
    // this width, not two.
    readonly property int stepperWidth: 30

    signal valueCommitted(int value)

    implicitHeight: ExoTheme.controlHeight
    editable: true
    focusPolicy: Qt.StrongFocus
    // up.hovered / down.hovered stay false without it, and the steppers then
    // have no resting-to-hover transition at all.
    hoverEnabled: true

    // The Basic style reserves a left slot for the down indicator and a right
    // slot for the up one. This field draws both on the right, so the inherited
    // bindings indent the value by a stepper width against nothing -- next to an
    // ExoSelect in the same column the two values start 26 px apart. Restated
    // here rather than left to the style: the padding has to follow where the
    // indicators actually are.
    leftPadding: 0
    rightPadding: root.stepperWidth

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

    // Why the current value is not accepted, or empty when it is. Shown as a
    // red frame and a warning mark inside the field, with the text as tooltip
    // on hover and keyboard focus and as the accessible description -- so an
    // error never adds a line under the row or moves the page.
    property string errorText: ""
    readonly property bool hasError: root.errorText !== ""

    ToolTip.visible: root.hasError && (root.hovered || root.activeFocus)
    ToolTip.text: root.errorText
    ToolTip.delay: 200
    Accessible.description: root.errorText

    contentItem: TextInput {
        text: root.displayText
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignLeft
        readOnly: !root.editable
        validator: root.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        selectByMouse: true
        // The number itself stays readable while locked; the stepper glyphs and
        // the frame below already carry that it cannot be changed.
        color: root.enabled ? ExoTheme.text : ExoTheme.textSecondary
        selectionColor: ExoTheme.accent
        selectedTextColor: ExoTheme.accentInk
        leftPadding: ExoTheme.spacingMd
        rightPadding: root.hasError ? 16 + ExoTheme.spacingSm : 0
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
        }
    }

    background: Rectangle {
        color: root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: 1
        border.color: root.hasError ? ExoTheme.error : root.activeFocus ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm

        ExoFieldErrorMark {
            visible: root.hasError
            x: parent.width - root.stepperWidth - width - ExoTheme.spacingSm
            anchors.verticalCenter: parent.verticalCenter
        }

        // The stepper column needs an edge of its own. Two 12 px glyphs on the
        // same fill as the value read as decoration next to the number rather
        // than as the two buttons they are, and nothing says where one ends and
        // the other begins.
        Rectangle {
            x: parent.width - root.stepperWidth
            width: 1
            height: parent.height - 2
            y: 1
            color: root.enabled ? ExoTheme.line : "transparent"
        }

        Rectangle {
            x: parent.width - root.stepperWidth
            width: root.stepperWidth - 1
            height: 1
            y: parent.height / 2
            color: root.enabled ? ExoTheme.line : "transparent"
        }
    }

    up.indicator: Item {
        x: root.width - width
        height: parent.height / 2
        implicitWidth: root.stepperWidth

        Rectangle {
            color: root.up.pressed ? ExoTheme.surfaceHover : ExoTheme.hoverTint(ExoTheme.surfaceRaised)
            visible: root.enabled && (root.up.pressed || root.up.hovered)
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
        implicitWidth: root.stepperWidth

        Rectangle {
            color: root.down.pressed ? ExoTheme.surfaceHover : ExoTheme.hoverTint(ExoTheme.surfaceRaised)
            visible: root.enabled && (root.down.pressed || root.down.hovered)
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
