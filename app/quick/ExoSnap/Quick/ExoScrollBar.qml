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
    padding: 3
    // Without this the handle shrinks to the bar's own thickness on a long list —
    // a 5000-entry log would leave a 12 px square to grab.
    minimumSize: control.orientation === Qt.Vertical
                 ? Math.min(1, 28 / Math.max(control.height, 1))
                 : Math.min(1, 28 / Math.max(control.width, 1))

    // No track: the handle alone is the quietest thing that still reads as a
    // scroll position, and the surfaces underneath already carry the page's own
    // surface layering.
    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6
        radius: Math.min(width, height) / 2
        color: control.pressed ? ExoTheme.textSecondary
             : control.hovered ? ExoTheme.textMuted
             : ExoTheme.textDim
        // Deliberately not the Basic style's fade-out: the gutter is reserved
        // whether or not the bar is drawn, so a bar that comes and goes would only
        // add motion, never room.
        visible: control.size < 1.0 || control.policy === ScrollBar.AlwaysOn
    }
}
