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

    signal committed(string value)

    function adoptValue(): void {
        if (root.text !== root.value)
            root.text = root.value;
    }

    implicitHeight: ExoTheme.controlHeight
    selectByMouse: true
    // Same rule as the locked select: the value stays readable, the field's
    // fill and border say it cannot be edited.
    color: root.enabled ? ExoTheme.text : ExoTheme.textSecondary
    placeholderTextColor: ExoTheme.textDim
    selectionColor: ExoTheme.accent
    selectedTextColor: ExoTheme.accentInk
    leftPadding: ExoTheme.spacingMd
    rightPadding: ExoTheme.spacingMd
    font {
        family: ExoTheme.sansFamily
        pixelSize: ExoTheme.fontBody
    }

    background: Rectangle {
        color: root.enabled ? ExoTheme.surfaceRaised : ExoTheme.surface
        border.width: 1
        border.color: root.activeFocus ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
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
