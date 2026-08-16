import QtQuick
import QtQuick.Controls.Basic

// The frontend's one scroll bar. ExoScrollView installs it on both axes; a bare
// Flickable or ListView assigns it to its own ScrollBar attached property, where
// the attached machinery does the positioning.
//
// The Basic style is imported deliberately, the same way every other Exo control
// does it: the default style on Windows draws this control natively and marks it
// non-customizable, which is why every scrolling surface came out carrying an
// opaque 17 px near-white groove instead of something belonging to the theme.
ScrollBar {
    id: control

    // 12 px overall against the native 17 px. Every surface that scrolls reserves
    // a spacingLg (16 px) gutter for the bar, so a narrower bar keeps that gutter
    // correct and still cannot reach the content.
    //
    // QCR-510. `padding: 3` used to inset the contentItem, which made the DRAG
    // TARGET the 6 px visual thumb — half the bar's own width and a third of
    // the native one. The padding is gone and the contentItem is the full 12 px
    // instead, with the visible thumb centred inside it at its original 6 px:
    // the target doubles, the drawing does not change by a pixel, and the
    // control's implicit size — and therefore every reserved gutter — is
    // exactly what it was.
    padding: 0
    // Without this the handle shrinks to the bar's own thickness on a long list —
    // a 5000-entry log would leave a 12 px square to grab.
    minimumSize: control.orientation === Qt.Vertical
                 ? Math.min(1, 28 / Math.max(control.height, 1))
                 : Math.min(1, 28 / Math.max(control.width, 1))

    // No track: the handle alone is the quietest thing that still reads as a
    // scroll position, and the surfaces underneath already carry the page's own
    // surface layering.
    contentItem: Item {
        implicitWidth: 12
        implicitHeight: 12
        // Deliberately not the Basic style's fade-out: the gutter is reserved
        // whether or not the bar is drawn, so a bar that comes and goes would only
        // add motion, never room.
        visible: control.size < 1.0 || control.policy === ScrollBar.AlwaysOn

        Rectangle {
            // The thumb the user sees. 6 px on the bar's cross axis, the full
            // length of the handle on the scroll axis.
            width: control.orientation === Qt.Vertical ? 6 : parent.width
            height: control.orientation === Qt.Vertical ? parent.height : 6
            anchors.centerIn: parent
            radius: 3
            color: control.pressed ? ExoTheme.textSecondary
                 : control.hovered ? ExoTheme.textMuted
                 : ExoTheme.textDim
        }
    }
}
