import QtQuick
import QtQuick.Controls.Basic

// The one menu in the design system.
//
// Both menus in this application — the preset overflow and the Record button's
// countdown — were plain `Menu` instances, which the Basic style paints white
// with black text. In a dark-by-default recorder that reads as a foreign dialog
// that has escaped from another application, and it was equally wrong in the two
// light themes, which are warm greys rather than pure white.
//
// Items are ExoMenuItem. A caller that writes `MenuItem` directly still gets the
// Basic look, so they are declared as ExoMenuItem at the call site rather than
// re-styled here through a delegate the caller cannot see.
Menu {
    id: root

    // In the window's own overlay: a native popup is a second top-level window,
    // which puts it outside `--visual-test`'s window grab and outside the frame
    // at the 860 px minimum window.
    popupType: Popup.Item
    padding: ExoTheme.spacingXs
    // Popups have no ambient light in this design language; the border does the
    // separating, the same way cards do.
    background: Rectangle {
        implicitWidth: 180
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusMd
    }
}
