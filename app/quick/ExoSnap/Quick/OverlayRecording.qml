pragma ComponentBehavior: Bound

import QtQuick

// The recording HUD: a compact status pill on the recorded source, absorbing
// what used to be OverlayDiagnostics.qml's separate window.
//
// Capture-excluded and unconditionally click-through.
//
// WHAT IT SAYS
// ------------
// A glyph carries the state and the text carries the facts. There is no REC or
// PAUSED word: the pill only ever appears while a capture is live, so the word
// would restate what the glyph already says, in the one place on screen where
// space is most expensive. The three production states are
//
//     recording   coral dot        the capture is running
//     paused      amber pause      the capture is held
//     warning     amber triangle   the capture is running AND frames were dropped
//
// There is no error state. A fatal failure removes this window and raises the
// recording-error surface instead; see models::ResolveRecordingOverlayState.
//
// WHICH TEXT APPEARS is a user preference resolved by
// models::OverlayContentPolicy, never decided here — this file binds the
// resolved booleans and lays them out.
//
// ONE WINDOW, TWO INDEPENDENT SECTIONS
// -------------------------------------
// The recording section (glyph, elapsed, output size, source name) and the
// diagnostics section (fps/drop/drift tokens, muted-source glyphs) are gated
// by two separate settings, exactly as they were as two windows: "recording
// off, diagnostics on" still shows only the diagnostics tokens. Merging the
// surface does not couple the settings. The status glyph and the elapsed
// clock stay anchored to the pill's right edge — the one thing a user is
// looking at during a capture never shifts position — and the diagnostics
// tokens are prepended to its LEFT, growing the pill leftward when present
// rather than pushing the clock sideways.
Window {
    id: root

    // Matched by role in observability/WindowIdentity and by the --visual-test
    // overlay grab (main.cpp), which both key off this name rather than the
    // window's title. Set here rather than by whoever instantiates the window,
    // so it holds regardless of whether that is Main.qml's own declaration or a
    // Loader created later -- the identity is the window's own, not the site
    // that happens to create it.
    objectName: "quickOverlayRecording"

    // ── Business inputs: recording section ───────────────────────────────────
    // Geometry of the actually-recorded source, in virtual-desktop coordinates
    // (OverlayAdapter::recordedSourceGeometry — the region rect in Region
    // mode, the live window rect in Window mode, the monitor rect in Monitor
    // mode). An empty rect falls back to this window's own screen.
    property rect monitorGeometry: Qt.rect(0, 0, 0, 0)
    // An OverlayAdapter.State value. Compared against that enum by name below
    // rather than against 1/2/3: the adapter exports the enum to QML precisely
    // so a value added or reordered in C++ cannot silently re-map what this
    // window paints, and a local mirror of the numbers is the copy that would
    // have to be found and updated by hand when it does.
    property int overlayState: OverlayAdapter.Hidden
    // The gate the recording side controls; capture exclusion gates on top of it.
    property bool overlayActive: false

    property string elapsedText: ""
    property string outputSizeText: ""
    property string sourceNameText: ""

    // Resolved content flags (SettingsAdapter -> models::OverlayContentPolicy).
    property bool showElapsed: true
    property bool showOutputSize: false
    property bool showSourceName: false

    // ── Business inputs: diagnostics section ─────────────────────────────────
    property bool diagnosticsActive: false

    property string fpsText: ""
    property string dropText: ""
    property string driftText: ""
    property bool micMuted: false
    property bool sysMuted: false

    property bool showFps: false
    property bool showDrop: true
    property bool showDrift: true
    // The diagnostics preset's own "size" toggle. Rendered through the same
    // outputSizeText field the recording section already carries rather than
    // as a second token: one value, shown once, if EITHER setting asks for it.
    property bool showDiagnosticsSize: false
    property bool showMutedSources: true

    readonly property bool showSize: root.showOutputSize || root.showDiagnosticsSize

    readonly property rect effectiveGeometry: root.monitorGeometry.width > 0 && root.monitorGeometry.height > 0
                                              ? root.monitorGeometry
                                              : Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)

    readonly property string unavailable: "—"

    // The configured diagnostics tokens, in a fixed reading order, LABELS ONLY.
    // Built here rather than as conditional Items so the separators can be
    // positioned from the list index, and kept as an array of labels rather
    // than resolved values so the array changes only when the CONTENT POLICY
    // changes (a user toggling a token in Settings) and not on every value tick
    // — a `var` property compares by identity, so a value-carrying array would
    // make every fps/drop/drift/size update a full model replacement, tearing
    // down and rebuilding the token delegates several times a second on the
    // same GUI thread as the DXGI preview and this pill's own clock.
    readonly property var diagnosticsTokens: {
        const list = [];
        if (root.showFps)
            list.push("fps");
        if (root.showDrop)
            list.push("drop");
        if (root.showDrift)
            list.push("drift");
        return list;
    }

    function diagnosticsTokenValue(label: string): string {
        switch (label) {
        case "fps":
            return root.fpsText;
        case "drop":
            return root.dropText;
        case "drift":
            return root.driftText;
        }
        return "";
    }

    readonly property bool anyMutedGlyph: root.showMutedSources && (root.micMuted || root.sysMuted)
    readonly property bool diagnosticsContentPresent: root.diagnosticsTokens.length > 0 || root.anyMutedGlyph

    // Paused and Warning used to share caution amber, which made a deliberate
    // pause look like a fault to a user glancing at the corner of a full-screen
    // game. Paused now takes the accent — the same colour the Resume action in
    // the transport carries — and amber is left to mean what it says.
    // The overlay* rungs, not the appearance ones: this pill's ground is
    // near-black whatever the application appearance is, so it resolves its
    // colours against the Dark appearance (see ExoTheme).
    readonly property color stateTone: {
        switch (root.overlayState) {
        case OverlayAdapter.Paused:
            return ExoTheme.overlayPaused;
        case OverlayAdapter.Warning:
            return ExoTheme.overlayWarning;
        default:
            return ExoTheme.overlayError;  // recording: the canonical rec tone
        }
    }

    // ── Overlay tokens ───────────────────────────────────────────────────────
    // The pill floats over arbitrary captured content, so its surface is darker
    // and more opaque than any in-app surface token and cannot come from
    // ExoTheme. Deliberately borderless: a hairline reads as a window edge over
    // moving content, which is exactly what this must not look like.
    //
    // Because that ground is fixed, everything drawn ON it takes the
    // `overlay*` rungs. The appearance ones were used here, and in Light they
    // resolve to dark ink: `ExoTheme.text` measured 1.09:1 against this pill.
    readonly property color pillBackground: "#C6161618"  // ~78% opaque near-black

    // No transient parent: a Window declared inside another Window inherits it
    // and then follows it into the tray on minimise. An overlay that disappears
    // when the app window is minimised fails exactly the case it exists for --
    // the user recording full-screen with ExoSnap out of the way.
    transientParent: null

    // Qt.WindowTransparentForInput is not conditional and has no property behind
    // it. This window sits over whatever the user is doing; taking a click would
    // be a defect, not a setting.
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus | Qt.WindowTransparentForInput

    // Named, and NOT left to Qt's default. An untitled QWindow falls back to the
    // application display name, which made every overlay a top-level window
    // titled "ExoSnap" -- six of them, indistinguishable from the main window to
    // anything that identifies it by owner pid and title. The staged updater does
    // exactly that when it asks the app to close for a swap.
    title: qsTr("ExoSnap Overlay — Recording")
    color: "transparent"

    // Fail-closed as a binding: `granted` is false until a platform call proved
    // otherwise, so no evaluation order can put this window on screen unexcluded.
    // Either section can carry the pill alone -- "recording off, diagnostics
    // on" is the same independence the two separate windows used to have.
    visible: exclusion.granted && (root.overlayActive || (root.diagnosticsActive && root.diagnosticsContentPresent))

    width: pill.implicitWidth
    height: pill.implicitHeight

    // Top-right of the source rect by preference, clamped so the pill never
    // extends past it: the product contract is that this pill sits ENTIRELY
    // inside the actually-recorded picture, and the diagnostics merge can
    // make it noticeably wider than a bare 20 px inset from the edge would
    // fit in a small Region or a small App window. When the pill itself is
    // wider or taller than the source rect, min > max below and Math.max
    // wins: anchored to the source's own top/left edge rather than centred
    // or left overflowing -- never a silent fall-back to the monitor.
    readonly property point clampedPosition: {
        const source = root.effectiveGeometry;
        const margin = 20;
        const minX = source.x;
        const maxX = source.x + source.width - width;
        const minY = source.y;
        const maxY = source.y + source.height - height;
        return Qt.point(Math.max(minX, Math.min(maxX, source.x + source.width - width - margin)),
                        Math.max(minY, Math.min(maxY, source.y + margin)));
    }

    x: root.clampedPosition.x
    y: root.clampedPosition.y

    CaptureExclusion {
        id: exclusion

        target: root
    }

    // Reserve the width of the longest clock the session can reach, so the pill
    // does not resize on every digit change — a pill that twitches once a second
    // over a game is more distracting than the recording indicator itself.
    TextMetrics {
        id: clockMetrics

        font: elapsedLabel.font
        text: "00:00:00"
    }

    // Vector glyphs rather than font characters: the mono faces in use render
    // the pause and warning code points inconsistently, and this pill has no
    // room for a fallback that is a different size.
    component StateGlyph: Canvas {
        id: glyph

        // "recording" | "paused" | "warning"
        property string kind: "recording"
        property color tone: root.stateTone

        // Only the recording dot breathes. Paused is held on purpose and a
        // warning must not look like it is about to go away.
        property real breath: 1.0

        width: 10
        height: 10
        opacity: glyph.kind === "recording" ? glyph.breath : 1.0

        // Animated here rather than followed from the shell's frames. This is a
        // scene graph, so a breathing dot costs nothing and can run for as long
        // as the recording does; the tray and the taskbar swap whole icons, so
        // theirs is a short transition instead. Same beat, two policies -- the
        // PERIOD is the canonical one, and only its amplitude is this surface's
        // own: at 10 px over arbitrary captured content, the shell frames' few
        // percent of opacity would not read at all.
        SequentialAnimation {
            running: glyph.kind === "recording"
            loops: Animation.Infinite

            NumberAnimation {
                target: glyph
                property: "breath"
                from: 1.0
                to: 0.55
                duration: Brand.recordingBeatMs / 2
                easing.type: Easing.InOutSine
            }

            NumberAnimation {
                target: glyph
                property: "breath"
                from: 0.55
                to: 1.0
                duration: Brand.recordingBeatMs / 2
                easing.type: Easing.InOutSine
            }
        }
        onKindChanged: requestPaint()
        onToneChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = glyph.tone;
            ctx.strokeStyle = glyph.tone;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";

            const cx = width / 2;
            const cy = height / 2;

            if (glyph.kind === "paused") {
                const bw = 2.4;
                const bh = 9;
                ctx.beginPath();
                ctx.rect(cx - 1.4 - bw, cy - bh / 2, bw, bh);
                ctx.fill();
                ctx.beginPath();
                ctx.rect(cx + 1.4, cy - bh / 2, bw, bh);
                ctx.fill();
            } else if (glyph.kind === "warning") {
                // Filled triangle with a punched-out bar and dot: legible at
                // 10 px in a way an outlined exclamation mark is not.
                ctx.beginPath();
                ctx.moveTo(cx, cy - 5);
                ctx.lineTo(cx + 5.2, cy + 4.4);
                ctx.lineTo(cx - 5.2, cy + 4.4);
                ctx.closePath();
                ctx.fill();
                ctx.globalCompositeOperation = "destination-out";
                ctx.beginPath();
                ctx.rect(cx - 0.7, cy - 1.8, 1.4, 3.4);
                ctx.fill();
                ctx.beginPath();
                ctx.rect(cx - 0.7, cy + 2.4, 1.4, 1.4);
                ctx.fill();
            } else {
                ctx.beginPath();
                ctx.arc(cx, cy, 4.2, 0, 2 * Math.PI, false);
                ctx.fill();
            }
        }
    }

    // Muted-source indicators are drawn rather than typed: the Widgets original
    // moved off font glyphs because combining characters rendered inconsistently
    // across the installed mono faces.
    component MutedGlyph: Canvas {
        id: glyph

        property string kind: "mic"
        property color tone: ExoTheme.overlayInkSecondary

        width: 15
        height: 15
        onKindChanged: requestPaint()
        onToneChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.strokeStyle = glyph.tone;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            ctx.lineWidth = 1.2;

            const cx = width / 2;
            const cy = height / 2;

            if (glyph.kind === "mic") {
                // Capsule body, U-shaped base, stem.
                ctx.beginPath();
                ctx.roundedRect(cx - 2.5, cy - 5.5, 5, 7, 2.5, 2.5);
                ctx.stroke();
                ctx.beginPath();
                ctx.arc(cx, cy + 1, 4.5, 0, Math.PI, false);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(cx, cy + 1.5);
                ctx.lineTo(cx, cy + 4.5);
                ctx.stroke();
            } else {
                // Speaker body plus cone.
                ctx.beginPath();
                ctx.rect(cx - 5, cy - 3, 3.5, 6);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(cx - 1.5, cy - 3);
                ctx.lineTo(cx + 2.5, cy - 4.8);
                ctx.lineTo(cx + 2.5, cy + 4.8);
                ctx.lineTo(cx - 1.5, cy + 3);
                ctx.stroke();
            }

            // Slash: top-right to bottom-left, marking the source as muted.
            ctx.lineWidth = 1.4;
            ctx.beginPath();
            ctx.moveTo(cx + 5.25, cy - 6);
            ctx.lineTo(cx - 5.25, cy + 6);
            ctx.stroke();
        }
    }

    Rectangle {
        id: pill

        implicitWidth: row.implicitWidth + 2 * 12
        implicitHeight: row.implicitHeight + 2 * 7
        anchors.fill: parent
        color: root.pillBackground
        radius: height / 2

        // Anchored to the pill's right edge rather than centred: the recording
        // glyph and clock are the rightmost group, and the diagnostics tokens
        // are prepended before them in the row's child order. Growing the
        // diagnostics section widens the row to the LEFT, leaving the clock's
        // own screen position untouched -- centering the whole row would instead
        // shift the clock sideways by half of whatever the diagnostics section
        // grew by.
        Row {
            id: row

            objectName: "overlayTokenRow"

            anchors {
                right: parent.right
                rightMargin: 12
                verticalCenter: parent.verticalCenter
            }
            spacing: 8

            Repeater {
                objectName: "overlayTokenRepeater"

                model: root.diagnosticsTokens

                delegate: Row {
                    id: tokenRow

                    required property string modelData
                    required property int index

                    readonly property string resolvedValue: {
                        const raw = root.diagnosticsTokenValue(tokenRow.modelData);
                        return raw.length > 0 ? raw : root.unavailable;
                    }
                    // Zero dropped frames is the one measured "all good" state
                    // this pill reports in green. Any other count stays neutral
                    // rather than alarming -- the diagnostics tone is calm,
                    // never alarmist, and a dropped frame is reported, not
                    // shouted about.
                    readonly property bool good: tokenRow.modelData === "drop" && root.dropText === "0"

                    visible: root.diagnosticsActive
                    spacing: 7

                    Text {
                        text: tokenRow.modelData
                        textFormat: Text.PlainText
                        color: ExoTheme.overlayInkMuted
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: 13
                        }
                    }

                    Text {
                        text: tokenRow.resolvedValue
                        textFormat: Text.PlainText
                        color: tokenRow.good ? ExoTheme.overlaySuccess : ExoTheme.overlayInk
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: 13
                        }
                    }

                    // Interpunct separator ahead of the NEXT token, carried by
                    // whichever element (a diagnostics token or the recording
                    // glyph) follows this one -- so the separator only appears
                    // between two things that are both actually showing.
                    Text {
                        visible: tokenRow.index < root.diagnosticsTokens.length - 1 || root.anyMutedGlyph
                                 || root.overlayActive
                        text: "·"
                        textFormat: Text.PlainText
                        color: ExoTheme.overlayInkMuted
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: 13
                        }
                    }
                }
            }

            // A Row owns its children's x; y stays free, so centre by hand
            // rather than anchoring into the positioner.
            MutedGlyph {
                kind: "mic"
                visible: root.diagnosticsActive && root.showMutedSources && root.micMuted
                y: (row.height - height) / 2
            }

            MutedGlyph {
                kind: "sys"
                visible: root.diagnosticsActive && root.showMutedSources && root.sysMuted
                y: (row.height - height) / 2
            }

            Text {
                visible: root.anyMutedGlyph && root.overlayActive
                text: "·"
                textFormat: Text.PlainText
                color: ExoTheme.overlayInkMuted
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 13
                }
            }

            StateGlyph {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.overlayActive
                kind: {
                    switch (root.overlayState) {
                    case OverlayAdapter.Paused:
                        return "paused";
                    case OverlayAdapter.Warning:
                        return "warning";
                    default:
                        return "recording";
                    }
                }
            }

            Text {
                id: elapsedLabel

                anchors.verticalCenter: parent.verticalCenter
                visible: root.overlayActive && root.showElapsed
                // Fixed to the widest clock so digit changes do not reflow the
                // pill; right-aligned inside that box so the colons stay put.
                width: visible ? clockMetrics.width : 0
                horizontalAlignment: Text.AlignRight
                text: root.elapsedText
                textFormat: Text.PlainText
                color: ExoTheme.overlayInk
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 13
                    weight: Font.Medium
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.overlayActive && root.showSize && root.outputSizeText.length > 0
                text: root.outputSizeText
                textFormat: Text.PlainText
                color: ExoTheme.overlayInkSecondary
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 13
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.overlayActive && root.showSourceName && root.sourceNameText.length > 0
                // Bounded: a window title can be arbitrarily long and this pill
                // must not grow across the recorded screen.
                width: Math.min(implicitWidth, 220)
                elide: Text.ElideRight
                text: root.sourceNameText
                textFormat: Text.PlainText
                color: ExoTheme.overlayInkSecondary
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: 13
                }
            }
        }
    }
}
