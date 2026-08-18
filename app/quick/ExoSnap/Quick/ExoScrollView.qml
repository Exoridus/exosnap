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

    // ── Automation-addressable scrolling (protocol 2) ────────────────────────
    //
    // ScrollView wraps content that is not itself flickable in a Flickable and
    // exposes it as `contentItem`; the clamp arithmetic below is that object's,
    // not an approximation of it.
    //
    // All three answer whether the surface REALLY ended up where it was asked
    // to. The control channel reports `settled` from that, because the defect
    // this replaces was a scroll request that silently did nothing while every
    // screenshot taken afterwards claimed to show the end of the page.
    readonly property Flickable flickable: control.contentItem as Flickable

    function scrollToHome(): bool {
        const view = control.flickable;
        if (view === null)
            return false;
        view.contentY = 0;
        return view.contentY === 0;
    }

    function scrollToEnd(): bool {
        const view = control.flickable;
        if (view === null)
            return false;
        const maximum = Math.max(0, view.contentHeight - view.height);
        view.contentY = maximum;
        return view.contentY === maximum;
    }

    // Keyboard scrolling, reported the same way the other helpers report: whether
    // the view actually moved. A key that changes nothing must not be swallowed --
    // Escape has to keep reaching the surface above, and Tab has to keep walking.
    function scrollByKey(key: int): bool {
        const view = control.flickable;
        if (view === null || view.height <= 0)
            return false;
        const page = view.height * 0.9;
        const line = view.height * 0.1;
        let delta = 0;
        if (key === Qt.Key_Down)
            delta = line;
        else if (key === Qt.Key_Up)
            delta = -line;
        else if (key === Qt.Key_PageDown)
            delta = page;
        else if (key === Qt.Key_PageUp)
            delta = -page;
        else if (key === Qt.Key_Home)
            return control.scrollToHome();
        else if (key === Qt.Key_End)
            return control.scrollToEnd();
        if (delta === 0)
            return false;
        const maximum = Math.max(0, view.contentHeight - view.height);
        const before = view.contentY;
        view.contentY = Math.max(0, Math.min(maximum, before + delta));
        return view.contentY !== before;
    }

    function revealItem(item: Item): bool {
        const view = control.flickable;
        if (view === null || item === null || view.height <= 0)
            return false;
        const top = item.mapToItem(view.contentItem, 0, 0).y;
        view.contentY = Math.min(Math.max(0, top), Math.max(0, view.contentHeight - view.height));
        // Not "we set contentY, therefore it worked": a target below a content
        // height that has not been laid out yet lands outside the viewport, and
        // saying so is the whole reason this returns a value.
        const relative = top - view.contentY;
        return relative >= 0 && relative < view.height;
    }

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
