import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: root

    // The canonical value the backing adapter holds. Bind this, NOT `text`.
    //
    // `text: adapter.something` looks equivalent and is not: typing into a
    // TextField does not break the binding on `text` (Qt only drops a binding on
    // an imperative JS write), so the next time the adapter emits its change
    // signal — a capability delivery, a config sanitize, any other field on the
    // same page being edited, all of which share one aggregate notify — the
    // binding re-evaluates and replaces whatever the user had typed so far. The
    // caret jumps to the end of a value they did not choose, mid-word.
    //
    // Here the editor owns its draft for as long as it has the focus, and the
    // canonical value is adopted at exactly three points: when the field is
    // built, when a change arrives while nobody is editing, and after a commit.
    // Nothing external can overwrite an edit in progress.
    property string value: ""

    // An optional action inside the field's right edge -- a folder picker, a
    // placeholder list -- drawn as a glyph from the shared set, never as text,
    // so every field action looks like one family. It acts on the field rather
    // than replacing it: typing stays possible.
    property int trailingGlyph: ExoGlyph.Invalid
    // Draws the glyph in this colour instead of the field's ink -- a Swatch
    // glyph showing the colour a hex field holds. Transparent keeps the ink.
    property color trailingGlyphColor: "transparent"
    property string trailingAccessibleName: ""
    // Kept highlighted while whatever it opened is showing.
    property bool trailingActive: false
    readonly property bool hasTrailingAction: root.trailingGlyph !== ExoGlyph.Invalid

    // A fixed sigil before the value, such as the '#' of a hex colour. Part of
    // the field's look, not of its value: it cannot be typed over or selected,
    // and `value` never contains it.
    property int leadingGlyph: ExoGlyph.Invalid
    readonly property Item trailingButton: trailingAction

    signal committed(string value)
    signal trailingClicked()

    // Why the current value is not accepted, or empty when it is. Shown as a
    // red frame and a warning mark inside the field, with the text as tooltip
    // on hover and keyboard focus and as the accessible description -- so an
    // error never adds a line under the row or moves the page.
    property string errorText: ""
    readonly property bool hasError: root.errorText !== ""

    function adoptValue(): void {
        if (root.text !== root.value)
            root.text = root.value;
    }

    implicitHeight: ExoTheme.controlHeight
    selectByMouse: true
    hoverEnabled: true
    ToolTip.visible: root.hasError && (root.hovered || root.activeFocus)
    ToolTip.text: root.errorText
    ToolTip.delay: 200
    Accessible.description: root.errorText
    // Same rule as the locked select: the value stays readable, the field's
    // fill and border say it cannot be edited.
    color: root.enabled ? ExoTheme.text : ExoTheme.textSecondary
    placeholderTextColor: ExoTheme.textDim
    selectionColor: ExoTheme.accent
    selectedTextColor: ExoTheme.accentInk
    leftPadding: root.leadingGlyph !== ExoGlyph.Invalid ? ExoTheme.spacingMd + leadingMark.width + ExoTheme.spacingXs
                                                        : ExoTheme.spacingMd
    rightPadding: (root.hasTrailingAction ? trailingAction.width + ExoTheme.spacingSm : ExoTheme.spacingMd)
                  + (root.hasError ? errorMark.width + ExoTheme.spacingXs : 0)
    font {
        family: ExoTheme.sansFamily
        pixelSize: ExoTheme.fontBody
    }

    background: Rectangle {
        color: root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: 1
        border.color: root.hasError ? ExoTheme.error : root.activeFocus ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
    }

    ExoGlyph {
        id: leadingMark

        kind: root.leadingGlyph
        visible: root.leadingGlyph !== ExoGlyph.Invalid
        color: ExoTheme.textMuted
        width: 14
        height: 14
        anchors.left: parent.left
        anchors.leftMargin: ExoTheme.spacingMd
        anchors.verticalCenter: parent.verticalCenter
    }

    ExoFieldErrorMark {
        id: errorMark

        visible: root.hasError
        anchors.right: root.hasTrailingAction ? trailingAction.left : parent.right
        anchors.rightMargin: root.hasTrailingAction ? ExoTheme.spacingXs : ExoTheme.spacingMd
        anchors.verticalCenter: parent.verticalCenter
    }

    AbstractButton {
        id: trailingAction

        visible: root.hasTrailingAction
        width: 28
        height: root.height - 8
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        enabled: root.enabled
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.Button
        Accessible.name: root.trailingAccessibleName
        onClicked: root.trailingClicked()

        HoverHandler {
            cursorShape: trailingAction.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        }

        background: Rectangle {
            radius: ExoTheme.radiusSm
            color: trailingAction.hovered || root.trailingActive ? ExoTheme.surfaceHover : "transparent"
            border.width: trailingAction.visualFocus ? ExoTheme.focusRingWidth : 0
            border.color: ExoTheme.text
        }

        contentItem: Item {
            ExoGlyph {
                kind: root.trailingGlyph
                visible: root.trailingGlyph !== ExoGlyph.Invalid
                color: root.trailingGlyphColor.a > 0 ? root.trailingGlyphColor
                     : trailingAction.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
                width: 14
                height: 14
                anchors.centerIn: parent
            }
        }
    }

    Component.onCompleted: root.adoptValue()

    onValueChanged: {
        if (!root.activeFocus)
            root.adoptValue();
    }

    // Leaving the field without committing — or committing something the
    // adapter refused or normalised — puts the canonical value back on screen.
    onActiveFocusChanged: {
        if (!root.activeFocus)
            root.adoptValue();
    }

    // Enter, or focus leaving the field. The adapter may sanitize what it was
    // given, so the field takes back whatever it actually holds afterwards.
    onEditingFinished: {
        root.committed(root.text);
        root.adoptValue();
    }
}
