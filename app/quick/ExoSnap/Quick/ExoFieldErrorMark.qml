import QtQuick

// The warning triangle a field shows inside its own frame while its value is
// invalid. The message itself is the field's tooltip and accessible
// description, so an error changes no row height and moves nothing on the page.
Item {
    implicitWidth: 16
    implicitHeight: 16

    ExoGlyph {
        kind: ExoGlyph.Warning
        color: ExoTheme.error
        anchors.fill: parent
    }
}
