import QtQuick

// The product name.
//
// A raster of `app/assets/brand/marks/wordmark.svg` rather than two Labels, so
// the name follows the running theme through the same substitution the marks do
// and reads identically wherever a font would have been substituted out from
// under it.
//
// The asset is Hanken Grotesk SemiBold converted to outlines, and its box is
// declared in units of the type size it was cut at -- so setting `typePixelSize`
// to the pixel size a Label would have used puts the name at that type size.
// Its box is padded above the letters so that centring the box centres the
// x-height band: `exosnap` has a descender and no ascender, and centring its ink
// instead hangs the name visibly low beside the round mark it sits next to.
Item {
    id: root

    // The type size the wordmark is drawn at, in the same units a Label's
    // `font.pixelSize` uses.
    property real typePixelSize: ExoTheme.fontBrand

    implicitHeight: Math.max(1, Math.round(root.typePixelSize * Brand.wordmarkEmHeight))
    implicitWidth: Math.round(root.implicitHeight * Brand.wordmarkAspect)

    Image {
        id: wordmark

        anchors.fill: parent

        // The device pixels the wordmark will occupy. Never zero: a provider
        // asked for a zero-pixel raster returns nothing, and the name would then
        // stay missing until something resized it.
        readonly property int rasterHeight: Math.max(1, Math.round(wordmark.height * Screen.devicePixelRatio))
        // The same arithmetic the renderer does, so `sourceSize` names the image
        // the provider will actually return and Qt Quick rescales nothing.
        readonly property int rasterWidth: Math.max(1, Math.round(wordmark.rasterHeight * Brand.wordmarkAspect))

        // The palette ids rather than resolved colours: the renderer reads the
        // same theme table the application does, and resolving them here would be
        // a second answer to one question.
        source: Brand.wordmarkSource(wordmark.rasterHeight, QuickThemeTokens.appearanceId, QuickThemeTokens.accentId)
        sourceSize: Qt.size(wordmark.rasterWidth, wordmark.rasterHeight)
        fillMode: Image.PreserveAspectFit
        smooth: true
    }
}
