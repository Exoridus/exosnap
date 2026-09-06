import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// Completeness gate for the shared icon set.
//
// The list below is the contract: every ExoGlyph.Kind a call site may name has
// to appear here AND resolve to geometry. Adding a kind without drawing it fails
// `hasGeometry`; adding one without listing it here fails `coversEveryKind`,
// because the table's length and this list's length are compared directly. There
// is deliberately no way to satisfy one without the other.
//
// This is the half of the gate that fails a build. qmllint does report a
// mistyped kind — `Member "Cihp" not found on type "ExoGlyph"` — but the Qt
// CMake lint target does not pass --max-warnings, so that report is a line in
// the log rather than a non-zero exit.
Item {
    id: root

    width: 64
    height: 64

    // Name -> value, written out rather than derived: a test that enumerated the
    // enum from the type under test could not detect a kind being renamed.
    readonly property var declaredKinds: ({
        "Invalid": ExoGlyph.Invalid,
        "Speaker": ExoGlyph.Speaker,
        "AppWindow": ExoGlyph.AppWindow,
        "Mic": ExoGlyph.Mic,
        "Webcam": ExoGlyph.Webcam,
        "Shutter": ExoGlyph.Shutter,
        "Flag": ExoGlyph.Flag,
        "Scissors": ExoGlyph.Scissors,
        "Pause": ExoGlyph.Pause,
        "Stop": ExoGlyph.Stop,
        "Record": ExoGlyph.Record,
        "Chip": ExoGlyph.Chip,
        "Check": ExoGlyph.Check,
        "Close": ExoGlyph.Close,
        "Search": ExoGlyph.Search,
        "Plus": ExoGlyph.Plus,
        "Minus": ExoGlyph.Minus,
        "Overflow": ExoGlyph.Overflow,
        "Display": ExoGlyph.Display,
        "Region": ExoGlyph.Region,
        "Dot": ExoGlyph.Dot,
        "Warning": ExoGlyph.Warning,
        "Info": ExoGlyph.Info,
        "ExternalLink": ExoGlyph.ExternalLink,
        "Back": ExoGlyph.Back,
        "Folder": ExoGlyph.Folder,
        "SplitTrack": ExoGlyph.SplitTrack,
        "Clock": ExoGlyph.Clock,
        "PreviewBroken": ExoGlyph.PreviewBroken,
        "Layers": ExoGlyph.Layers,
        "Copy": ExoGlyph.Copy,
        "Refresh": ExoGlyph.Refresh,
        "Run": ExoGlyph.Run,
        "Send": ExoGlyph.Send,
        "ArrowRight": ExoGlyph.ArrowRight
    })

    Component {
        id: glyphComponent

        ExoGlyph {
            width: 18
            height: 18
        }
    }

    TestCase {
        name: "ExoGlyphTests"
        when: windowShown

        function test_everyDeclaredKindHasGeometry() {
            for (const name in root.declaredKinds) {
                const value = root.declaredKinds[name];
                verify(value !== undefined, "ExoGlyph." + name + " is not a declared enum value");
                const glyph = createTemporaryObject(glyphComponent, root, { kind: value });
                verify(!!glyph, "ExoGlyph." + name + " instantiates");
                if (name === "Invalid") {
                    // The one kind that must draw nothing, so a mistyped
                    // reference — which QML coerces to 0 — renders no icon and
                    // logs, instead of silently rendering the wrong one.
                    compare(glyph.pathData, "", "ExoGlyph.Invalid must have no geometry");
                } else {
                    verify(glyph.pathData.length > 0, "ExoGlyph." + name + " has no geometry");
                    verify(glyph.pathData.indexOf("M") === 0,
                           "ExoGlyph." + name + " geometry must be an SVG path");
                }
            }
        }

        function test_coversEveryKind() {
            const glyph = createTemporaryObject(glyphComponent, root, { kind: ExoGlyph.Check });
            verify(!!glyph, "Component exists");
            compare(glyph.kindCount, Object.keys(root.declaredKinds).length,
                    "A kind was added to ExoGlyph.Kind or to its geometry table without the other");
        }

        function test_kindOutsideTheTableDrawsNothing() {
            const glyph = createTemporaryObject(glyphComponent, root, { kind: 9999 });
            verify(!!glyph, "Component exists");
            compare(glyph.pathData, "");
        }

        function test_filledKindsAreFilledAndOutlinedKindsAreNot() {
            const filled = [ExoGlyph.Pause, ExoGlyph.Stop, ExoGlyph.Overflow, ExoGlyph.Dot];
            for (const kind of filled) {
                const glyph = createTemporaryObject(glyphComponent, root, { kind: kind });
                verify(glyph.filled, "kind " + kind + " must be filled");
            }
            for (const kind of [ExoGlyph.Check, ExoGlyph.Search, ExoGlyph.Chip, ExoGlyph.Record]) {
                const glyph = createTemporaryObject(glyphComponent, root, { kind: kind });
                verify(!glyph.filled, "kind " + kind + " must be stroked");
            }
        }

        // The M-prefix check above passes for a single degenerate point, so
        // ArrowRight gets a real geometric check: a shaft plus an angled head
        // are two separate subpaths, the head is drawn with diagonal (not just
        // axis-aligned) strokes, and the whole glyph covers a plausible span of
        // the 18x18 grid rather than collapsing to a corner or a dot.
        function test_arrowRightIsAShaftWithAHead() {
            const glyph = createTemporaryObject(glyphComponent, root, { kind: ExoGlyph.ArrowRight });
            verify(!!glyph, "Component exists");

            const path = glyph.pathData;
            const subpaths = path.split("M").filter(function (segment) { return segment.trim().length > 0; });
            compare(subpaths.length, 2, "ArrowRight must draw the shaft and the head as separate subpaths");
            verify(path.indexOf("L") >= 0, "ArrowRight's head must use angled strokes, not only H/V lines");

            const coords = path.match(/-?\d+(?:\.\d+)?/g).map(Number);
            const min = Math.min(...coords);
            const max = Math.max(...coords);
            verify(max - min >= 8,
                   "ArrowRight geometry must span a plausible fraction of the 18x18 grid, spanned " + (max - min));
        }
    }
}
