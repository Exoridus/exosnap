pragma ComponentBehavior: Bound

import QtQuick

// The recording HUD: a compact status pill on the recorded monitor.
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
Window {
    id: root

    // ── Business inputs ──────────────────────────────────────────────────────
    // Geometry of the monitor being recorded, in virtual-desktop coordinates.
    // An empty rect falls back to this window's own screen, the way the Widgets
    // overlays fell back to the primary screen before a target was resolved.
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

    readonly property rect effectiveGeometry: root.monitorGeometry.width > 0 && root.monitorGeometry.height > 0
                                              ? root.monitorGeometry
                                              : Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)

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
            return ExoTheme.overlayAccent;
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
    // when the app window is minimised fails exactly the case it exists for —
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
    visible: exclusion.granted && root.overlayActive

    width: pill.implicitWidth
    height: pill.implicitHeight
    x: root.effectiveGeometry.x + root.effectiveGeometry.width - width - 20
    y: root.effectiveGeometry.y + 20

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

        width: 10
        height: 10
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

    Rectangle {
        id: pill

        implicitWidth: row.implicitWidth + 2 * 12
        implicitHeight: row.implicitHeight + 2 * 7
        anchors.fill: parent
        color: root.pillBackground
        radius: height / 2

        Row {
            id: row

            anchors.centerIn: parent
            spacing: 8

            StateGlyph {
                anchors.verticalCenter: parent.verticalCenter
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
                visible: root.showElapsed
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
                visible: root.showOutputSize && root.outputSizeText.length > 0
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
                visible: root.showSourceName && root.sourceNameText.length > 0
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
