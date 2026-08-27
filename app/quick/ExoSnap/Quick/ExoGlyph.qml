import QtQuick
import QtQuick.Shapes

// The shared line-icon set.
//
// Drawn rather than typeset. A UI symbol set as a text character — "✓", "✕",
// "⌕", "⋯" — depends on the bundled font carrying that codepoint; Hanken Grotesk
// carries almost none of them, so Qt silently substituted a system font. What
// then appeared had neither this set's stroke weight nor its optical size, it
// differed from machine to machine, and where no installed font had the
// codepoint at all it was an empty box. Nothing about that failure is visible in
// the source, which is why it survived several visual passes.
//
// Drawn rather than shipped as .svg FILES, too: every icon here is recoloured
// constantly — accent, error, textMuted, textDim, and accentInk on a filled
// button — and an `Image` cannot be tinted without one file per colour or an
// effect node per icon. `PathSvg` takes the same path data a design tool
// exports, while `strokeColor` stays an ordinary bindable property.
//
// Geometry is authored on an 18x18 grid and scaled by the item's own size, with
// the stroke divided back out, so a 14 px icon is the same shape at the same
// stroke weight as a 20 px one rather than a thinner version of it.
Item {
    id: root

    // Every kind must have an entry in `_paths` at its own index — the order
    // here IS the table order below. `tst_ExoGlyph.qml` fails if the two ever
    // disagree, so a new kind cannot be declared without being drawn.
    //
    // `Invalid` is deliberately zero, and deliberately drawn as nothing. A
    // mistyped reference — `ExoGlyph.Serach` — evaluates to `undefined`, and QML
    // coerces that to 0 on an int property. With a real glyph at zero the typo
    // would render the WRONG icon and say nothing; with Invalid there it renders
    // no icon and logs which kind was asked for. The lint pass reports the typo
    // too (verified: `Member "Cihp" not found on type "ExoGlyph"`), but it
    // reports it as a warning the Qt CMake lint target does not fail on, so this
    // is the half that does not depend on anyone reading the output.
    enum Kind {
        Invalid,
        Speaker,
        AppWindow,
        Mic,
        Webcam,
        Shutter,
        Flag,
        Scissors,
        Pause,
        Stop,
        Record,
        Chip,
        Check,
        Close,
        Search,
        Plus,
        Minus,
        Overflow,
        Dot,
        Warning,
        Info,
        Display,
        Region,
        ExternalLink,
        Back,
        Folder,
        SplitTrack
    }

    // An ExoGlyph.Kind value. Declared `int` because a QML-declared enum is not
    // a property type of its own; call sites still write `ExoGlyph.Search`, and
    // the lint pass rejects a name that is not a declared value.
    // (A comment must not open with the linter's own name — it reads the rest of
    //  the line as a directive.)
    required property int kind
    property color color: "white"
    property real strokeWidth: 1.5

    implicitWidth: 18
    implicitHeight: 18

    // ── The set ─────────────────────────────────────────────────────────────
    //
    // Indexed by Kind. Kept as data rather than as a switch so that "declared
    // but not drawn" is a value a test can see, instead of a branch that
    // silently falls through to nothing.
    readonly property var _paths: [
        // Invalid — drawn as nothing, on purpose. See the Kind enum.
        "",
        // Speaker — cone plus two waves: system audio.
        "M2 7H5L8.5 3.5V14.5L5 11H2Z M10.99 6.49A3.2 3.2 0 0 1 10.99 11.51 M12.48 4.61A5.6 5.6 0 0 1 12.48 13.39",
        // AppWindow — a titled window: application audio follows one window. The
        // two marks in the title bar are what separates it from a plain framed
        // box at 18 px; without them it reads as a generic rectangle.
        "M2.5 3.5H15.5V14.5H2.5Z M2.5 7H15.5 M4.65 5.25H5.15 M7.25 5.25H7.75",
        // Mic — capsule, cradle, stem.
        "M6.5 5A2.5 2.5 0 0 1 11.5 5V8.5A2.5 2.5 0 0 1 6.5 8.5Z M14 9A5 5 0 0 1 4 9 M9 14V16",
        // Webcam — round body on a stand, with the lens drawn as a concentric
        // ring. A camcorder body reads as "recording" in general, which is the
        // one thing this toggle must not claim: it names the camera source
        // beside three audio sources, and it is off far more often than on.
        "M3.8 8.2A5.2 5.2 0 1 0 14.2 8.2A5.2 5.2 0 1 0 3.8 8.2"
            + " M6.8 8.2A2.2 2.2 0 1 0 11.2 8.2A2.2 2.2 0 1 0 6.8 8.2"
            + " M9 13.4V15.3 M5.8 15.3H12.2",
        // Shutter — still camera: capture one frame out of the recording.
        "M2.5 6H6L7 4H11L12 6H15.5V14H2.5Z M6.2 10A2.8 2.8 0 1 0 11.8 10A2.8 2.8 0 1 0 6.2 10",
        // Flag — marker.
        "M4.5 2.5V15.5 M4.5 3.5H13.5L11 6.75L13.5 10H4.5Z",
        // Scissors — split the recording into a new file.
        "M5 3L13 13 M13 3L5 13 M2.5 14.5A2 2 0 1 0 6.5 14.5A2 2 0 1 0 2.5 14.5"
            + " M11.5 14.5A2 2 0 1 0 15.5 14.5A2 2 0 1 0 11.5 14.5",
        // Pause (filled).
        "M5 4H8V14H5Z M10 4H13V14H10Z",
        // Stop (filled).
        "M5 5H13V13H5Z",
        // Record — outline, not a filled disc: on the accent-filled Record pill
        // a solid dot reads as a hole punched in the button.
        "M4.4 9A4.6 4.6 0 1 0 13.6 9A4.6 4.6 0 1 0 4.4 9",
        // Chip — package, die, three pins a side. The Device page drew a square
        // inside a square, which is the shape of a generic placeholder.
        "M4 4H14V14H4Z M7 7H11V11H7Z"
            + " M6.5 4V1.5 M9 4V1.5 M11.5 4V1.5 M6.5 14V16.5 M9 14V16.5 M11.5 14V16.5"
            + " M4 6.5H1.5 M4 9H1.5 M4 11.5H1.5 M14 6.5H16.5 M14 9H16.5 M14 11.5H16.5",
        // Check — "on", "supported", "passed", "included".
        "M3.8 9.4L7.4 13L14.2 5.4",
        // Close — dismiss, and the same mark for "unsupported" / "failed" /
        // "not included". One shape, because it IS one shape; the call site says
        // which of the two it means.
        "M4.6 4.6L13.4 13.4 M13.4 4.6L4.6 13.4",
        // Search.
        "M12.5 7.5A5 5 0 1 0 2.5 7.5A5 5 0 1 0 12.5 7.5 M11.05 11.05L15.5 15.5",
        // Plus / Minus — the number field's stepper.
        "M9 4.2V13.8 M4.2 9H13.8",
        "M4.2 9H13.8",
        // Overflow (filled) — the "more actions" menu.
        "M2.4 9A1.5 1.5 0 1 0 5.4 9A1.5 1.5 0 1 0 2.4 9 M7.5 9A1.5 1.5 0 1 0 10.5 9A1.5 1.5 0 1 0 7.5 9"
            + " M12.6 9A1.5 1.5 0 1 0 15.6 9A1.5 1.5 0 1 0 12.6 9",
        // Dot (filled) — a neutral marker where Check would claim too much.
        "M6.6 9A2.4 2.4 0 1 0 11.4 9A2.4 2.4 0 1 0 6.6 9",
        // Warning — triangle and bang. The bang's dot is a round-capped stub
        // rather than a filled circle, so the whole glyph stays one stroked path.
        "M9 3.2L16 15.2H2Z M9 7.6V11.2 M9 12.9V13.3",
        // Info.
        "M14.5 9A5.5 5.5 0 1 0 3.5 9A5.5 5.5 0 1 0 14.5 9 M9 8.2V12.8 M9 5.4V5.9",
        // Display — panel plus stand: a whole monitor as a capture target.
        // Distinct from AppWindow above, which has a title bar and no stand,
        // because the preview toolbar has to say which of the two is being
        // recorded with nothing but the icon.
        "M1.8 3.5H16.2V12H1.8Z M6.2 15.5H11.8 M9 12V15.5",
        // Region — four marquee corners: a rectangle the user drew, so it is
        // deliberately not a closed shape.
        "M2.5 6.2V2.5H6.2 M11.8 2.5H15.5V6.2 M15.5 11.8V15.5H11.8 M6.2 15.5H2.5V11.8",
        // ExternalLink — an open frame with an arrow leaving through its corner:
        // this action leaves ExoSnap for the browser. Deliberately not the same
        // shape as AppWindow, which is a capture TARGET inside the product.
        "M10.5 3.5H15.5V8.5 M15.5 3.5L9 10 M13 10.5V14.5H3.5V5H7.5",
        // Back -- the return-key arrow: a stroke that comes down the right, turns
        // left and ends in a head. A plain left arrow says "previous" (the item
        // before this one); this shape says "go back out of here", which is what
        // the completed transport's action does. Not a circular-arrow "undo"
        // either: nothing is undone, the recording stays where it was written.
        "M14.5 4.5V10.5H5.5 M9.5 6.5L5.5 10.5L9.5 14.5",
        // Folder -- a tabbed folder, closed. Deliberately not an open one: the
        // action reveals a file that is already there, it does not put one in.
        "M2.5 5.2V14H15.5V6.8H9L7.4 5.2Z",
        // SplitTrack -- one track cut into two pieces, with the cut drawn through
        // the gap. Replaces a pair of scissors, which says TRIM: an edit that
        // removes something from one file. This action removes nothing and edits
        // nothing -- it closes the current output and opens the next one, and the
        // recording carries on either way.
        "M2.5 6.4H7V11.6H2.5Z M11 6.4H15.5V11.6H11Z M9 3.4V14.6"
    ]

    // Kinds drawn as a filled silhouette instead of a stroked outline.
    readonly property var _filledKinds: [ExoGlyph.Pause, ExoGlyph.Stop, ExoGlyph.Overflow, ExoGlyph.Dot]

    // Empty for a kind outside the table. Read by tst_ExoGlyph to prove every
    // declared kind is drawn.
    readonly property string pathData: root.kind >= 0 && root.kind < root._paths.length
                                       ? root._paths[root.kind] : ""
    readonly property int kindCount: root._paths.length
    readonly property bool filled: root._filledKinds.indexOf(root.kind) >= 0

    readonly property real _scale: Math.max(0.01, Math.min(root.width, root.height) / 18)

    // Loud, but only in the log, and only when an icon was actually meant to be
    // seen. `Invalid` on a hidden instance is the ordinary "this control has no
    // icon" case — RecordActionButton, RecordSourceToggle and ExoButton all keep
    // a glyph around and hide it. `Invalid` on a VISIBLE one is either a kind
    // nobody drew or a mistyped reference that QML coerced to zero, and that is
    // the case worth shouting about.
    //
    // A log line rather than an abort: this is a defect the completeness test
    // and the lint pass are both meant to catch before a build ships, and an
    // 18 px icon is never a reason to take down an application that may be
    // recording.
    //
    // Armed only once the object is complete. `kind` is declared above
    // `pathData`, so during construction the kind-changed handler can run while
    // pathData still holds its default empty string and its own binding has not
    // been installed yet. Without this guard that reported a perfectly
    // well-drawn glyph as missing, once, on every start — a false alarm in the
    // shipped log is worse than no alarm, because it teaches the reader to skip
    // the line.
    property bool _complete: false

    onKindChanged: root.warnIfUndrawn()
    onVisibleChanged: root.warnIfUndrawn()
    Component.onCompleted: {
        root._complete = true;
        root.warnIfUndrawn();
    }

    function warnIfUndrawn(): void {
        if (root._complete && root.visible && root.pathData.length === 0)
            console.error("ExoGlyph: no geometry declared for kind", root.kind);
    }

    Shape {
        width: 18
        height: 18
        anchors.centerIn: parent
        scale: root._scale
        // Analytic antialiasing rather than relying on the window's multisample
        // configuration: these are 14-20 px line icons, and on a surface without
        // MSAA the geometry renderer leaves them visibly stepped.
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            // Divided back out of the item scale, so the stroke is the same
            // number of pixels at every icon size.
            strokeWidth: root.strokeWidth / root._scale
            strokeColor: root.filled ? "transparent" : root.color
            fillColor: root.filled ? root.color : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathSvg {
                path: root.pathData
            }
        }
    }
}
