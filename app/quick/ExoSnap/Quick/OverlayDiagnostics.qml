pragma ComponentBehavior: Bound

import QtQuick

// The diagnostics HUD: a live metrics readout on the recorded monitor, under
// the recording pill.
//
// Capture-excluded and unconditionally click-through.
//
// VOCABULARY
// ----------
// The labels are the short diagnostics tokens the rest of the product already
// uses — fps, drop, drift, size — rather than sentences. This pill is read at a
// glance during a capture, and the Diagnostics page carries the long form.
//
// TRUTHFULNESS
// ------------
// A token that is configured on but not yet measured renders as an em dash,
// never as zero: "not measured" and "measured zero" are different statements and
// the second one is the interesting one for drop. Which tokens appear at all is
// a user preference resolved by models::OverlayContentPolicy; every token in
// that policy has a named runtime producer, so a configured-on token can only
// ever be unmeasured, never unproduceable.
//
// The visual language is shared with OverlayRecording (same surface, same
// radius, borderless) rather than carried over from the Widgets original, whose
// hairline border and pure-white values were its own thing.
Window {
    id: root

    // ── Business inputs ──────────────────────────────────────────────────────
    property rect monitorGeometry: Qt.rect(0, 0, 0, 0)
    property bool overlayActive: false

    property string fpsText: ""
    property string dropText: ""
    property string driftText: ""
    property string sizeText: ""
    property bool micMuted: false
    property bool sysMuted: false

    // Resolved content flags (SettingsAdapter -> models::OverlayContentPolicy).
    property bool showFps: false
    property bool showDrop: true
    property bool showDrift: true
    property bool showSize: false
    property bool showMutedSources: true

    readonly property rect effectiveGeometry: root.monitorGeometry.width > 0 && root.monitorGeometry.height > 0
                                              ? root.monitorGeometry
                                              : Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)

    readonly property string unavailable: "—"

    // The configured tokens, in a fixed reading order. Built here rather than as
    // four conditional Items so the separators below can be positioned from the
    // list index — with per-token `visible` flags, the separator logic is where
    // the bugs live.
    //
    // LABELS ONLY, deliberately. This used to carry each token's resolved value
    // as well, which made the array depend on fpsText/dropText/driftText/sizeText
    // — four properties that move on the diagnostics cadence, roughly four times
    // a second while recording. A `var` property compares by identity, so every
    // one of those was a model ASSIGNMENT: QQmlDelegateModel removed every row
    // and re-inserted it, tearing down and rebuilding two to four Rows and six to
    // twelve Texts (each with a fresh font-metric layout) per second, on the same
    // GUI thread as the DXGI preview and the transport clock.
    //
    // Now the array changes only when the CONTENT POLICY changes — a user
    // toggling a token in Settings — and a value moving is an ordinary property
    // update inside a delegate that stays alive.
    readonly property var tokens: {
        const list = [];
        if (root.showFps)
            list.push("fps");
        if (root.showDrop)
            list.push("drop");
        if (root.showDrift)
            list.push("drift");
        if (root.showSize)
            list.push("size");
        return list;
    }

    // The value behind a token label, unresolved. Kept as a function on the root
    // so the delegate binds to the four source properties directly and each
    // delegate re-evaluates only when its own token's text moves.
    function tokenValue(label: string): string {
        switch (label) {
        case "fps":
            return root.fpsText;
        case "drop":
            return root.dropText;
        case "drift":
            return root.driftText;
        case "size":
            return root.sizeText;
        }
        return "";
    }

    readonly property bool anyMutedGlyph: root.showMutedSources && (root.micMuted || root.sysMuted)

    // ── Overlay tokens ───────────────────────────────────────────────────────
    // See OverlayRecording.qml: the surface has to hold up over arbitrary
    // captured content, so it is not an ExoTheme surface token.
    // Same value as the recording pill: the two sit on the same edge, one under
    // the other, and two different opacities would read as two different things.
    // Everything drawn on it takes the `overlay*` ink rungs, for the reason
    // OverlayRecording states: the appearance rungs turn dark in Light.
    readonly property color pillBackground: "#C6161618"

    // The recording pill's height plus the gap between the two. This one always
    // sits underneath it, on the same right edge.
    readonly property int recPillOffset: 30 + 8

    // See OverlayRecording.qml.
    transientParent: null

    // Not conditional: an overlay over someone else's screen never takes input.
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus | Qt.WindowTransparentForInput
    color: "transparent"

    // Fail-closed: `granted` starts false. The empty-content case is already
    // resolved in C++ (OverlayAdapter never activates an empty pill), and the
    // guard is repeated here so a future direct instantiation cannot draw one.
    visible: exclusion.granted && root.overlayActive && (root.tokens.length > 0 || root.anyMutedGlyph)

    width: pill.implicitWidth
    height: pill.implicitHeight
    x: root.effectiveGeometry.x + root.effectiveGeometry.width - width - 20
    y: root.effectiveGeometry.y + 20 + root.recPillOffset

    CaptureExclusion {
        id: exclusion

        target: root
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

        Row {
            id: row

            // Named so a test can reach the token delegates and assert that a
            // value update keeps them alive rather than rebuilding the row.
            objectName: "overlayTokenRow"

            anchors.centerIn: parent
            spacing: 7

            Repeater {
                // Named for the same reason the row is: a test reaches the token
                // delegates by index to prove they survive a value update.
                objectName: "overlayTokenRepeater"

                model: root.tokens

                delegate: Row {
                    id: tokenRow

                    required property string modelData
                    required property int index

                    readonly property string resolvedValue: {
                        const raw = root.tokenValue(tokenRow.modelData);
                        return raw.length > 0 ? raw : root.unavailable;
                    }
                    // Zero dropped frames is the one measured "all good" state
                    // this pill reports in green. Any other count stays neutral
                    // rather than alarming — the diagnostics tone is calm, never
                    // alarmist, and a dropped frame is reported, not shouted
                    // about.
                    readonly property bool good: tokenRow.modelData === "drop" && root.dropText === "0"

                    spacing: 7

                    Text {
                        // Interpunct separator, carried by the token that
                        // FOLLOWS it — so the first token never has one and no
                        // trailing separator can survive a hidden last token.
                        visible: tokenRow.index > 0
                        text: "·"
                        textFormat: Text.PlainText
                        color: ExoTheme.overlayInkMuted
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: 13
                        }
                    }

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
                }
            }

            // A Row owns its children's x; y stays free, so centre by hand
            // rather than anchoring into the positioner.
            MutedGlyph {
                kind: "mic"
                visible: root.showMutedSources && root.micMuted
                y: (row.height - height) / 2
            }

            MutedGlyph {
                kind: "sys"
                visible: root.showMutedSources && root.sysMuted
                y: (row.height - height) / 2
            }
        }
    }
}
