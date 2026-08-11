import QtQuick
import QtQuick.Controls.Basic

// Every scrolling surface in the frontend uses this instead of a bare ScrollView.
// It is the single place the themed bar is installed, and the single place the
// edge geometry is written down: assigning a ScrollBar to a ScrollView replaces
// the style's own instance, and with it the parenting, position and length the
// style had supplied.
ScrollView {
    id: control

    // Surfaces whose content WIDTH feeds back into its own HEIGHT must set this.
    //
    // The default below makes the reserved strip depend on whether a bar is
    // currently drawn, i.e. on the content size. That is fine for a list, and a
    // binding loop for a centred, word-wrapped card: contentWidth follows
    // availableWidth, availableWidth follows the vertical bar, the vertical bar
    // follows contentHeight, contentHeight is the content's implicitHeight, and
    // that re-wraps against the width the cycle started from. Reserving both
    // gutters unconditionally cuts the width->height edge. No bar becomes
    // visible as a result -- ExoScrollBar hides its handle at size >= 1.0, so
    // this reserves space, it does not draw anything.
    property bool reserveScrollBarGutters: false

    // Same rule as the style this replaces: the reserved strip is exactly the
    // bar's own width and collapses to zero when no bar is drawn. Surfaces that
    // want a wider, unconditional gutter override these.
    rightPadding: control.reserveScrollBarGutters ? control.ScrollBar.vertical.implicitWidth
                                                  : control.effectiveScrollBarWidth
    bottomPadding: control.reserveScrollBarGutters ? control.ScrollBar.horizontal.implicitHeight
                                                   : control.effectiveScrollBarHeight

    ScrollBar.vertical: ExoScrollBar {
        parent: control
        x: control.mirrored ? 0 : control.width - width
        y: control.topPadding
        height: control.availableHeight
        active: control.ScrollBar.horizontal.active
    }

    ScrollBar.horizontal: ExoScrollBar {
        parent: control
        x: control.leftPadding
        y: control.height - height
        width: control.availableWidth
        active: control.ScrollBar.vertical.active
    }
}
