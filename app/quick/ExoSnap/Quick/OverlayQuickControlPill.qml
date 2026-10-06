pragma ComponentBehavior: Bound

import QtQuick

// Draggable quick-control pill: pause/resume, stop, capture frame.
// This is the one capture-excluded overlay that is NOT click-through: it is
// interactive by design, so it deliberately omits
// Qt.WindowTransparentForInput. Capture exclusion still applies unchanged — the
// controls must not burn into the recording either.
Window {
    id: root

    // See OverlayRecording.qml: set on the window itself so its identity holds
    // regardless of what instantiates it.
    objectName: "quickOverlayQuickControls"

    // ── Business inputs ──────────────────────────────────────────────────────
    // Single gate, resolved in C++ (OverlayAdapter::quickControlsActive) from
    // the "Show quick controls" setting AND the live capture state. The port
    // carried these as two separate properties because the Widgets class had two
    // setters; keeping them apart here would put the AND in the delegate.
    property bool overlayActive: false
    property bool paused: false
    property bool expanded: true

    // The RECORDED monitor's full rectangle, resolved in C++ from the live
    // capture target (OverlayAdapter::recordedMonitorGeometry), never from QML
    // screen enumeration.
    //
    // The Widgets class put this pill on the primary display because it had no
    // setMonitorGeometry() at all; the port carried that forward as an open
    // product question. It is settled now: controls belong on the screen the user
    // is looking at, which during a capture is the one being captured. The pill
    // is capture-excluded, so putting it there costs the recording nothing.
    //
    // The full monitor rectangle, not the work area: the dock may be moved all
    // the way to the real screen edge and may overlap the ordinary taskbar
    // there on purpose (see edgeMargin below) -- there is no enforced
    // taskbar safety margin.
    property rect monitorGeometry: Qt.rect(0, 0, 0, 0)

    // The actually-recorded source rectangle (OverlayAdapter::recordedSourceGeometry),
    // used only to prefer a placement near the recorded area -- never as a
    // movement boundary. A dock that could only move inside the source rect
    // could never sit BELOW an Area selection that reaches toward the bottom
    // of its monitor.
    property rect sourceGeometry: Qt.rect(0, 0, 0, 0)
    // True only when sourceGeometry is a Region-mode crop (OverlayAdapter::
    // recordedSourceIsRegion), as opposed to a Window-mode rectangle that also
    // happens to be smaller than its monitor. Only a region gets the
    // "prefer below/above it" placement; a recorded window gets the plain
    // bottom-of-monitor default a Monitor target already had.
    property bool sourceIsRegion: false

    readonly property rect effectiveMonitor: root.monitorGeometry.width > 0
                                             && root.monitorGeometry.height > 0
                                             ? root.monitorGeometry
                                             : Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)

    readonly property color pillBackground: "#CC0C0C0E"  // rgba(12,12,14,0.8)
    readonly property color pillBorder: "#1AFFFFFF"
    readonly property color gripTone: "#80FFFFFF"        // rgba(255,255,255,0.5)
    readonly property color buttonHover: "#14FFFFFF"
    readonly property color buttonPressed: "#24FFFFFF"
    readonly property color buttonGlyph: "#E6FFFFFF"       // rgba(255,255,255,0.9)
    // Stop is rec-styled: the coral tone, tinted for fill and border. The
    // `overlayError` rung, because this pill is near-black in both appearances
    // and Light's `error` lands at 4.15:1 on it against the Dark rung's 6.66:1.
    readonly property color stopBackground: Qt.alpha(ExoTheme.overlayError, 0.18)
    readonly property color stopBorder: Qt.alpha(ExoTheme.overlayError, 0.35)

    readonly property int pad: 6
    readonly property int gripWidth: 32
    readonly property int buttonSize: 44
    readonly property int buttonGap: 6
    readonly property int buttonSurfaceSize: 36
    readonly property int secondarySurfaceSize: 18
    readonly property int secondaryGlyphSize: 14
    readonly property int secondaryRightInset: 2
    // Include the Canvas stroke extents when spacing the visible glyphs.
    readonly property real closeInkHeight: root.secondaryGlyphSize * 0.5 + 1.6
    readonly property real gripInkHeight: root.secondaryGlyphSize * 0.45 + 1.8
    readonly property real secondaryGlyphGap: (root.height - root.closeInkHeight - root.gripInkHeight) / 3
    readonly property real closeGlyphCenterY: root.secondaryGlyphGap + root.closeInkHeight / 2
    readonly property real gripGlyphCenterY: root.height - root.secondaryGlyphGap - root.gripInkHeight / 2

    // A resting offset for the DEFAULT placement only -- not an enforced
    // safety margin. There is no forced taskbar exclusion: the dock may be
    // dragged all the way flush to the real monitor edge (see gripArea's drag
    // clamp below, whose floor is 0), and may overlap the ordinary taskbar
    // there on purpose. A small edge offset still reads better as "placed"
    // than a default position flush against the corner.
    readonly property int edgeMargin: ExoTheme.spacingSm
    // The gap between a Region-mode source rect and the dock's preferred
    // below/above placement, distinct from edgeMargin: this one separates two
    // things the user drew and can see move independently, not a screen edge.
    readonly property int regionGap: ExoTheme.spacingSm

    // Where the dock rests before it has ever been dragged, or after the
    // recording target changes and it has not been dragged yet: below a
    // Region-mode selection when there is room, above it when there is not,
    // and bottom-centre of the target monitor otherwise -- the same default a
    // Monitor or Window target always had. Always clamped to the monitor
    // rectangle, which is also the drag boundary.
    function clampToMonitor(x, y) {
        const area = root.effectiveMonitor;
        return Qt.point(
            Math.max(area.x, Math.min(area.x + area.width - root.width, x)),
            Math.max(area.y, Math.min(area.y + area.height - root.height, y)));
    }

    function defaultPosition() {
        const area = root.effectiveMonitor;
        if (root.sourceIsRegion && root.sourceGeometry.width > 0 && root.sourceGeometry.height > 0) {
            const region = root.sourceGeometry;
            const centeredX = region.x + (region.width - root.width) / 2;
            const belowY = region.y + region.height + root.regionGap;
            if (belowY + root.height <= area.y + area.height)
                return root.clampToMonitor(centeredX, belowY);
            const aboveY = region.y - root.regionGap - root.height;
            if (aboveY >= area.y)
                return root.clampToMonitor(centeredX, aboveY);
            // Neither fits (the region spans the monitor's full height):
            // falls through to the plain monitor default below, same as a
            // Monitor or Window target -- acceptable since this dock is
            // capture-excluded even when it lands inside the recorded area.
        }
        return root.clampToMonitor(area.x + (area.width - root.width) / 2,
                                   area.y + area.height - root.height - root.edgeMargin);
    }

    readonly property point defaultPos: root.defaultPosition()
    // Sticks once a real drag has moved the pill: only then does a monitor
    // change need to re-clamp a user-chosen offset rather than recompute the
    // default from scratch.
    property bool userPositioned: false
    property real previousWidth: 0

    signal pauseResumeRequested()
    signal stopRequested()
    signal captureFrameRequested()
    // Closing the dock is a persistent choice, not a per-session hide: the
    // handler in Main.qml turns the "show quick controls" setting off, so a
    // user who closes it once does not have to close it again next session.
    signal closeRequested()

    // See OverlayRecording.qml: an inherited transient parent would take the
    // pill down with the app window — and the pill exists precisely for sessions
    // where the app window is out of the way.
    transientParent: null

    // No Qt.WindowTransparentForInput here: this window takes mouse input.
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus

    // Named rather than left to Qt's default: an untitled QWindow inherits the
    // application display name, and five overlays titled "ExoSnap" made the main
    // window impossible to identify by owner pid and title. See OverlayRecording.
    title: qsTr("ExoSnap Overlay — Quick controls")

    color: "transparent"

    visible: exclusion.granted && root.overlayActive

    width: root.pad + root.gripWidth + root.pad
           + (root.expanded ? 3 * root.buttonSize + 3 * root.buttonGap : 0)
    height: root.pad + root.buttonSurfaceSize + root.pad

    // defaultPosition() by default. Dragging the grip assigns x/y directly,
    // which replaces these bindings — intentional: once the user has placed
    // the pill, it stays where they put it (see userPositioned below for what
    // happens to that placement across a monitor change).
    x: root.defaultPos.x
    y: root.defaultPos.y

    // A user-placed pill does not re-run defaultPosition() when the recording
    // target's monitor changes (dragging already overwrote the x/y bindings
    // above), so its offset has to be re-clamped by hand or it could sit
    // off-screen on a smaller monitor, or simply not on the new target's
    // monitor at all. Not re-run for an autoplaced pill: that one already
    // tracks the new default through the live x/y bindings.
    function reclampUserPosition() {
        if (!root.userPositioned)
            return;
        const clamped = root.clampToMonitor(root.x, root.y);
        root.x = clamped.x;
        root.y = clamped.y;
    }

    onEffectiveMonitorChanged: root.reclampUserPosition()
    onWidthChanged: {
        // Keep the right-hand grip under the pointer when a placed dock toggles.
        if (root.userPositioned && root.previousWidth > 0)
            root.x += root.previousWidth - root.width;
        root.previousWidth = root.width;
        root.reclampUserPosition();
    }
    onHeightChanged: root.reclampUserPosition()
    Component.onCompleted: root.previousWidth = root.width

    CaptureExclusion {
        id: exclusion

        target: root
    }

    // All pill glyphs are vector-drawn, matching the Widgets original: they are
    // sized to the 18 px nominal glyph box rather than a font's cap height.
    component PillGlyph: Canvas {
        id: glyph

        // "pause" | "resume" | "stop" | "camera" | "grip" | "close"
        property string kind: "pause"
        property color tone: root.buttonGlyph

        width: 18
        height: 18
        onKindChanged: requestPaint()
        onToneChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = glyph.tone
            ctx.fillStyle = glyph.tone
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.lineWidth = 1.6

            const cx = width / 2
            const cy = height / 2
            const s = Math.min(width, height) / 2

            if (glyph.kind === "pause") {
                const bw = s * 0.30
                const bh = s * 0.80
                const gap = s * 0.22
                ctx.beginPath()
                ctx.roundedRect(cx - gap / 2 - bw, cy - bh, bw, bh * 2, 1.5, 1.5)
                ctx.fill()
                ctx.beginPath()
                ctx.roundedRect(cx + gap / 2, cy - bh, bw, bh * 2, 1.5, 1.5)
                ctx.fill()
            } else if (glyph.kind === "resume") {
                ctx.beginPath()
                ctx.moveTo(cx - s * 0.3, cy - s * 0.6)
                ctx.lineTo(cx + s * 0.6, cy)
                ctx.lineTo(cx - s * 0.3, cy + s * 0.6)
                ctx.closePath()
                ctx.fill()
            } else if (glyph.kind === "stop") {
                const side = s * 0.85
                ctx.beginPath()
                ctx.roundedRect(cx - side / 2, cy - side / 2, side, side, 3, 3)
                ctx.fill()
            } else if (glyph.kind === "camera") {
                ctx.beginPath()
                ctx.roundedRect(cx - s * 0.65, cy - s * 0.4, s * 1.3, s, 3, 3)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(cx, cy + s * 0.1, s * 0.35, 0, 2 * Math.PI, false)
                ctx.stroke()
                ctx.beginPath()
                ctx.roundedRect(cx - s * 0.25, cy - s * 0.53, s * 0.5, s * 0.25, 2, 2)
                ctx.stroke()
            } else if (glyph.kind === "close") {
                ctx.lineWidth = 1.6
                ctx.beginPath()
                ctx.moveTo(cx - s * 0.5, cy - s * 0.5)
                ctx.lineTo(cx + s * 0.5, cy + s * 0.5)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(cx + s * 0.5, cy - s * 0.5)
                ctx.lineTo(cx - s * 0.5, cy + s * 0.5)
                ctx.stroke()
            } else {
                // Grip: three short horizontal lines.
                ctx.lineWidth = 1.8
                for (let i = -1; i <= 1; ++i) {
                    ctx.beginPath()
                    ctx.moveTo(cx - s * 0.5, cy + i * s * 0.45)
                    ctx.lineTo(cx + s * 0.5, cy + i * s * 0.45)
                    ctx.stroke()
                }
            }
        }
    }

    component PillButton: Item {
        id: button

        property string glyphKind: "pause"
        property bool recStyled: false
        property int surfaceSize: root.buttonSurfaceSize
        property int glyphSize: 18
        property real contentCenterY: button.height / 2
        property color glyphTone: button.recStyled ? ExoTheme.overlayError : root.buttonGlyph

        signal activated()

        width: root.buttonSize
        height: root.buttonSize

        Accessible.role: Accessible.Button
        Accessible.onPressAction: button.activated()

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: button.contentCenterY - height / 2
            width: button.surfaceSize
            height: button.surfaceSize
            radius: 10
            color: buttonMouse.pressed ? root.buttonPressed
                                      : buttonMouse.containsMouse ? root.buttonHover
                                                                 : button.recStyled ? root.stopBackground : "transparent"
            border.width: button.recStyled ? 1 : 0
            border.color: root.stopBorder
        }

        PillGlyph {
            anchors.horizontalCenter: parent.horizontalCenter
            y: button.contentCenterY - height / 2
            width: button.glyphSize
            height: button.glyphSize
            kind: button.glyphKind
            tone: button.glyphTone
        }

        MouseArea {
            id: buttonMouse

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: button.activated()
        }

    }

    Rectangle {
        anchors.fill: parent
        color: root.pillBackground
        border.width: 1
        border.color: root.pillBorder
        radius: 14

        Item {
            id: grip
            objectName: "quickControlGrip"

            Accessible.role: Accessible.Button
            Accessible.name: root.expanded ? qsTr("Collapse quick controls") : qsTr("Expand quick controls")
            Accessible.description: qsTr("Drag to move quick controls. Click to collapse or expand.")
            Accessible.onPressAction: root.expanded = !root.expanded

            x: root.width - root.secondaryRightInset - root.gripWidth
            y: root.height / 2
            width: root.gripWidth
            height: root.height / 2

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: root.gripGlyphCenterY - grip.y - height / 2
                width: root.secondarySurfaceSize
                height: root.secondarySurfaceSize
                radius: 6
                color: gripArea.pressed ? root.buttonPressed
                                        : gripArea.containsMouse ? root.buttonHover : "transparent"
            }

            PillGlyph {
                anchors.horizontalCenter: parent.horizontalCenter
                y: root.gripGlyphCenterY - grip.y - height / 2
                width: root.secondaryGlyphSize
                height: root.secondaryGlyphSize
                kind: "grip"
                tone: root.gripTone
            }

            MouseArea {
                id: gripArea

                // Where inside the window the pointer grabbed, in window
                // coordinates. Constant for the whole drag, and the reason the
                // drag is expressed as an absolute placement rather than as a
                // stream of relative nudges: assigning root.x moves the window
                // out from under a pointer that has not itself moved, and
                // Windows answers that with another move event reporting the
                // same desktop position. A relative step would re-apply its own
                // displacement on every one of those, so a single small gesture
                // accelerated until the clamp below caught it -- which is how a
                // few pixels of drag ended with the pill in a corner.
                property point grabOffset: Qt.point(0, 0)
                // Where the pointer was when the press landed, in
                // virtual-desktop coordinates, so a click can be told from a
                // drag without depending on how many events the gesture arrived
                // as.
                property point pressGlobal: Qt.point(0, 0)
                property real travelled: 0
                property bool dragging: false

                anchors.fill: parent
                hoverEnabled: true
                cursorShape: gripArea.dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                acceptedButtons: Qt.LeftButton
                onPressed: mouse => {
                    gripArea.grabOffset = Qt.point(mouse.x + grip.x, mouse.y + grip.y)
                    gripArea.pressGlobal = Qt.point(root.x + gripArea.grabOffset.x,
                                                    root.y + gripArea.grabOffset.y)
                    gripArea.travelled = 0
                    gripArea.dragging = true
                }
                onPositionChanged: mouse => {
                    if (!gripArea.dragging)
                        return
                    // Both read before either is written: assigning root.x
                    // would otherwise shift the frame the y reading is taken in.
                    const pointerX = root.x + grip.x + mouse.x
                    const pointerY = root.y + grip.y + mouse.y
                    gripArea.travelled = Math.max(gripArea.travelled,
                                                  Math.abs(pointerX - gripArea.pressGlobal.x)
                                                  + Math.abs(pointerY - gripArea.pressGlobal.y))
                    // Preserve the default-position bindings until the gesture
                    // becomes a drag; tiny click motion must not detach them.
                    if (gripArea.travelled <= 4)
                        return
                    root.userPositioned = true
                    // Clamped to the full monitor rectangle, not the work area:
                    // there is no enforced taskbar safety margin, so a manual
                    // drag may place the pill flush against the real screen
                    // edge and over the ordinary taskbar there. Floor is 0,
                    // not edgeMargin -- that margin is a resting offset for
                    // the DEFAULT placement, not a drag limit.
                    const area = root.effectiveMonitor
                    root.x = Math.max(area.x,
                                      Math.min(area.x + area.width - root.width,
                                               pointerX - gripArea.grabOffset.x))
                    root.y = Math.max(area.y,
                                      Math.min(area.y + area.height - root.height,
                                               pointerY - gripArea.grabOffset.y))
                }
                onCanceled: gripArea.dragging = false
                onReleased: {
                    gripArea.dragging = false
                    // A press that never really moved is a click on the grip:
                    // collapse or expand instead of nudging the pill by a pixel.
                    if (gripArea.travelled <= 4)
                        root.expanded = !root.expanded
                }
            }
        }

        Row {
            objectName: "quickControlActions"
            anchors {
                left: parent.left
                leftMargin: root.pad
                verticalCenter: parent.verticalCenter
            }
            spacing: root.buttonGap
            visible: root.expanded

            PillButton {
                glyphKind: root.paused ? "resume" : "pause"
                Accessible.name: root.paused ? qsTr("Resume recording") : qsTr("Pause recording")
                onActivated: root.pauseResumeRequested()
            }

            PillButton {
                glyphKind: "stop"
                recStyled: true
                Accessible.name: qsTr("Stop recording")
                onActivated: root.stopRequested()
            }

            PillButton {
                glyphKind: "camera"
                Accessible.name: qsTr("Capture frame")
                onActivated: root.captureFrameRequested()
            }
        }

        PillButton {
            objectName: "quickControlClose"
            x: grip.x
            width: root.gripWidth
            height: root.height / 2
            surfaceSize: root.secondarySurfaceSize
            glyphSize: root.secondaryGlyphSize
            contentCenterY: root.closeGlyphCenterY
            glyphTone: root.gripTone
            glyphKind: "close"
            Accessible.name: qsTr("Hide quick controls")
            Accessible.description: qsTr("Enable quick controls again in Settings.")
            onActivated: root.closeRequested()
        }
    }
}
