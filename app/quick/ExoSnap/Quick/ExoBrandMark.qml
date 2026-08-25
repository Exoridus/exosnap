import QtQuick

// The ExoSnap mark.
//
// The drawing is `app/assets/brand/marks/brand.svg` -- the same asset the tray
// and the taskbar button render -- recoloured for the running theme on its way
// through the shell image provider. It is not drawn here, and its coordinates
// are not repeated here: one mark, one source, four surfaces.
//
// Rasterized at the size it is displayed at rather than scaled from a fixed one:
// the provider applies the optical correction the target size needs, which is
// the whole reason a 16 px mark and a 48 px mark are not the same image scaled.
// An Item rather than a bare Image for the same reason: an Image takes its
// implicit size from its source, and the source here is chosen FROM the size.
Item {
    id: root

    implicitWidth: 18
    implicitHeight: 18

    Image {
        id: mark

        anchors.fill: parent

        // The device pixels the mark will occupy. Never zero: a provider asked
        // for a zero-pixel raster returns nothing, and the mark would then stay
        // missing until something resized it.
        readonly property int rasterSize: Math.max(1, Math.round(Math.min(mark.width, mark.height) * Screen.devicePixelRatio))

        // The palette ids rather than resolved colours: the renderer reads the
        // same theme table the application does, and resolving them here would be
        // a second answer to one question.
        source: Brand.source(mark.rasterSize, QuickThemeTokens.appearanceId, QuickThemeTokens.accentId)
        sourceSize: Qt.size(mark.rasterSize, mark.rasterSize)
        fillMode: Image.PreserveAspectFit
        smooth: true
    }
}
