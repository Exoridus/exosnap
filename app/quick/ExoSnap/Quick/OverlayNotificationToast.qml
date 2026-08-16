pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes

// Transient notification toasts, stacked bottom-right of the screen that hosts
// the ExoSnap window. Ported from app/ui/overlay/NotificationToastWindow.cpp.
//
// Input model: one translucent window spans the whole stack, so the transparent
// gaps between cards would swallow clicks meant for the app behind. Rather than
// Qt.WindowTransparentForInput — which would also kill the action buttons — the
// window keeps a mask covering only the card rectangles, rebuilt whenever the
// stack changes. That is why this overlay is only partially click-through.
//
// The card delegate below follows product-spec §9's toast rules exactly. They
// used to live in a second, never-instantiated reference component
// (NotificationToastCard.qml, removed in QCR-701); this file is now the only
// place a toast is described, so the rules belong here:
//  - a dismiss ✕ is always present (NotificationToastWindow::ToastHit's
//    is_dismiss target exists independently of action count);
//  - with exactly one action the whole card is clickable, marked with a
//    trailing "›";
//  - with two actions each gets its own named (quiet) button;
//  - the body wraps up to six lines and ellipsizes beyond that — the hub,
//    not the toast, is where the untruncated text lives;
//  - a countdown bar renders only for a TIMED toast (`standing: false`),
//    matching NotificationManager::IsStanding()/DismissIntervalMs().
Window {
    id: root

    // ── Business inputs (the lead wires these to the notification manager) ────
    // A model of visible toasts. Expected roles, one entry per card:
    //   title (string), body (string), tone ("success"|"caution"|"error"|"info"),
    //   standing (bool), remainingFraction (real 0..1), sequence (int),
    //   actionCount (int), primaryLabel/primaryAction (string),
    //   secondaryLabel/secondaryAction (string)
    property var toasts: null
    // Available geometry (taskbar excluded) of the screen hosting the app window.
    property rect anchorGeometry: Qt.rect(0, 0, 0, 0)

    readonly property rect effectiveGeometry: root.anchorGeometry.width > 0 && root.anchorGeometry.height > 0
                                              ? root.anchorGeometry
                                              : Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)

    // ── Geometry (design source: ToastAnatomy) ───────────────────────────────
    readonly property int cardWidth: 372
    // The window is wider than the card on every side so the soft shadow's
    // penumbra has room; the cards sit inset by this margin.
    readonly property int shadowMargin: 20
    readonly property int stackGap: 12

    signal actionTriggered(int sequence, int action)
    signal dismissRequested(int sequence)

    // See OverlayRecording.qml: an inherited transient parent would take the
    // toasts down with the app window, and toasts about a finished recording are
    // most useful exactly when the app is minimised.
    transientParent: null

    // No Qt.WindowTransparentForInput: the mask above is what keeps the gaps
    // between cards click-through, and the flag would defeat the cards too.
    // It was set here regardless, which made every toast unoperable on the real
    // desktop -- dismiss, Edit and Show in folder all dead, and a toast the user
    // could not get rid of. WindowDoesNotAcceptFocus stays: the toast must not
    // steal focus from whatever is being recorded, and it does not prevent
    // clicks (OverlayQuickControlPill ships the same combination).
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus
    color: "transparent"

    visible: exclusion.granted && repeater.count > 0

    width: root.cardWidth + 2 * root.shadowMargin
    height: stack.height + 2 * root.shadowMargin
    x: root.effectiveGeometry.x + root.effectiveGeometry.width - width - 20
    y: root.effectiveGeometry.y + root.effectiveGeometry.height - height - 20

    CaptureExclusion {
        id: exclusion

        target: root
    }

    // Union of the card rectangles, each grown by the shadow penumbra so those
    // semi-transparent pixels still composite. Everything outside falls through
    // to the window behind.
    function rebuildMask() {
        const rects = []
        for (let i = 0; i < repeater.count; ++i) {
            const card = repeater.itemAt(i)
            if (!card)
                continue
            rects.push(Qt.rect(stack.x + card.x - root.shadowMargin,
                               stack.y + card.y - 10,
                               card.width + 2 * root.shadowMargin,
                               card.height + 48))
        }
        exclusion.setClickThroughRegion(rects)
    }

    onVisibleChanged: if (visible) root.rebuildMask()

    // Status glyph inside the 30 px chip. Vector-drawn so it stays identical
    // across the mono/sans faces the rest of the card uses.
    component StatusGlyph: Canvas {
        id: statusGlyph

        property string tone: "info"
        property color stroke: ExoTheme.accent

        width: 16
        height: 16
        onToneChanged: requestPaint()
        onStrokeChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = statusGlyph.stroke
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.lineWidth = 1.5

            const cx = width / 2
            const cy = height / 2
            const r = width / 2 - 1

            if (statusGlyph.tone === "caution") {
                // Alert triangle.
                ctx.beginPath()
                ctx.moveTo(cx, cy - r)
                ctx.lineTo(cx + r, cy + r * 0.8)
                ctx.lineTo(cx - r, cy + r * 0.8)
                ctx.closePath()
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(cx, cy - r * 0.25)
                ctx.lineTo(cx, cy + r * 0.25)
                ctx.stroke()
                return
            }

            ctx.beginPath()
            ctx.arc(cx, cy, r, 0, 2 * Math.PI, false)
            ctx.stroke()

            if (statusGlyph.tone === "success") {
                ctx.beginPath()
                ctx.moveTo(cx - r * 0.4, cy + r * 0.05)
                ctx.lineTo(cx - r * 0.1, cy + r * 0.38)
                ctx.lineTo(cx + r * 0.42, cy - r * 0.28)
                ctx.stroke()
            } else if (statusGlyph.tone === "error") {
                ctx.beginPath()
                ctx.moveTo(cx - r * 0.35, cy - r * 0.35)
                ctx.lineTo(cx + r * 0.35, cy + r * 0.35)
                ctx.moveTo(cx + r * 0.35, cy - r * 0.35)
                ctx.lineTo(cx - r * 0.35, cy + r * 0.35)
                ctx.stroke()
            } else {
                // Info: the "i" stem and dot.
                ctx.beginPath()
                ctx.moveTo(cx, cy - r * 0.1)
                ctx.lineTo(cx, cy + r * 0.45)
                ctx.moveTo(cx, cy - r * 0.45)
                ctx.lineTo(cx, cy - r * 0.4)
                ctx.stroke()
            }
        }
    }

    component ActionPill: Rectangle {
        id: pill

        property string label: ""
        property bool primary: false
        property color tone: ExoTheme.accent
        property color ink: ExoTheme.accentInk

        signal activated()

        implicitWidth: pillLabel.implicitWidth + 24
        implicitHeight: 28
        radius: 10
        color: pill.primary ? pill.tone : "transparent"
        border.width: pill.primary ? 0 : 1
        border.color: Qt.alpha(pill.tone, 0.45)

        Accessible.role: Accessible.Button
        Accessible.name: pill.label
        Accessible.onPressAction: pill.activated()

        Text {
            id: pillLabel

            anchors.centerIn: parent
            text: pill.label
            textFormat: Text.PlainText
            color: pill.primary ? pill.ink : ExoTheme.text
            font {
                family: ExoTheme.sansFamily
                pixelSize: 13
                weight: Font.DemiBold
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: pill.activated()
        }
    }

    Column {
        id: stack

        x: root.shadowMargin
        y: root.shadowMargin
        width: root.cardWidth
        spacing: root.stackGap

        onHeightChanged: root.rebuildMask()

        Repeater {
            id: repeater

            model: root.toasts

            onCountChanged: root.rebuildMask()

            delegate: Rectangle {
                id: card

                required property var model

                readonly property color tone: ExoTheme.advisoryTone(card.model.tone)
                readonly property int actionCount: card.model.actionCount !== undefined ? card.model.actionCount : 0
                // One action means the card itself is the action, marked with a
                // chevron; two get named buttons in their own row.
                readonly property bool cardIsAction: card.actionCount === 1
                // The countdown bar appears exactly when the toast leaves on its
                // own — a standing toast reports a condition that still holds.
                readonly property bool standing: card.model.standing === true

                width: root.cardWidth
                height: 14 + Math.max(30, textBlock.height + (actionRow.visible ? 11 + actionRow.height : 0))
                        + 14 + (card.standing ? 0 : 3)
                radius: 14
                color: ExoTheme.surfaceHover
                border.width: 1
                border.color: ExoTheme.lineStrong

                Accessible.role: Accessible.AlertMessage
                Accessible.name: card.model.title
                Accessible.description: card.model.body

                Rectangle {
                    id: chip

                    x: 16
                    y: 14
                    width: 30
                    height: 30
                    radius: 9
                    // The tone tints, never floods: the card keeps its own
                    // surface so a caution toast does not read as an alarm.
                    color: Qt.alpha(card.tone, 0.13)
                    border.width: 1
                    border.color: Qt.alpha(card.tone, 0.45)

                    StatusGlyph {
                        anchors.centerIn: parent
                        tone: card.model.tone
                        stroke: ExoTheme.advisoryToneText(card.model.tone)
                    }
                }

                Column {
                    id: textBlock

                    x: 58
                    // Centre the title+body block on the chip while it is
                    // shorter than the chip; a wrapped body just starts at the
                    // top padding.
                    y: 14 + Math.max(0, (30 - height) / 2)
                    // Clear the ✕ column, and the chevron column too when the
                    // card carries one — otherwise the text wraps straight
                    // through it.
                    width: root.cardWidth - 58 - 15 - 18 - 6 - (card.cardIsAction ? 24 : 0)
                    spacing: 3

                    Text {
                        width: parent.width
                        text: card.model.title
                        textFormat: Text.PlainText
                        color: ExoTheme.text
                        elide: Text.ElideRight
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: 14
                            weight: Font.DemiBold
                        }
                    }

                    Text {
                        width: parent.width
                        visible: text.length > 0
                        text: card.model.body !== undefined ? card.model.body : ""
                        textFormat: Text.PlainText
                        color: ExoTheme.textMuted
                        // WrapAnywhere as the fallback, not WordWrap alone: a
                        // file path is one unbreakable token, and WordWrap has
                        // no legal break in it -- so the line simply grew past
                        // the card and out of the window, with elide never
                        // reached because eliding only applies to the last of
                        // several lines. Any body that cannot wrap now breaks
                        // mid-token instead of overrunning.
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        // Grow-to-fit up to six lines; beyond that the last line
                        // elides. The notification hub always keeps the full text.
                        maximumLineCount: 6
                        elide: Text.ElideRight
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: 13
                        }
                    }
                }

                Row {
                    id: actionRow

                    x: 58
                    y: textBlock.y + textBlock.height + 11
                    spacing: 8
                    visible: card.actionCount >= 2

                    ActionPill {
                        label: card.model.primaryLabel !== undefined ? card.model.primaryLabel : ""
                        primary: true
                        tone: card.tone
                        ink: ExoTheme.advisoryToneInk(card.model.tone)
                        onActivated: root.actionTriggered(card.model.sequence, card.model.primaryAction)
                    }

                    ActionPill {
                        label: card.model.secondaryLabel !== undefined ? card.model.secondaryLabel : ""
                        tone: card.tone
                        onActivated: root.actionTriggered(card.model.sequence, card.model.secondaryAction)
                    }
                }

                // Single-action affordance: the whole card is the target, the
                // chevron only marks it.
                ExoChevron {
                    x: root.cardWidth - 15 - 18 - 6 - 18
                    y: (card.height - (card.standing ? 0 : 3) - height) / 2
                    width: 14
                    height: 14
                    visible: card.cardIsAction
                    direction: 270
                    tone: ExoTheme.textMuted
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: card.cardIsAction
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.actionTriggered(card.model.sequence, card.model.primaryAction)
                }

                // The dismiss affordance. It was a bare `Text` with no `text` at
                // all: the hit target, the hover colour and the accessible name
                // were all there and correct, and the glyph itself was simply
                // never drawn — an 18 px hole in the corner of every desktop
                // toast that only a user who guessed could click. Drawn with the
                // shared ExoGlyph, the same treatment the hub's own dismiss
                // uses, so the two surfaces cannot drift; the 18 px target is
                // unchanged.
                Item {
                    id: dismiss

                    x: root.cardWidth - 15 - 18
                    y: 14
                    width: 18
                    height: 18

                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Dismiss notification")
                    Accessible.onPressAction: root.dismissRequested(card.model.sequence)

                    ExoGlyph {
                        anchors.centerIn: parent
                        width: 12
                        height: 12
                        kind: ExoGlyph.Close
                        color: dismissArea.containsMouse ? ExoTheme.text : ExoTheme.textDim
                    }

                    MouseArea {
                        id: dismissArea

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.dismissRequested(card.model.sequence)
                    }
                }

                // Countdown hairline along the bottom edge of timed toasts.
                //
                // A clipping band with the CARD's own outline drawn inside it,
                // not a 3 px Rectangle laid over the bottom edge. A Rectangle
                // clamps its corner radius to half its shortest side, so at 3 px
                // tall it can round its own corners by 1.5 px against the card's
                // 14 — while the card's bottom arc cuts about 5 px inwards over
                // exactly those three rows. The bar's square ends therefore hung
                // outside the card's rounded corners, which is the broken edge
                // this replaces.
                //
                // The Widgets toast this was ported from clipped the bar to the
                // card path (setClipPath); the shape is the same one, described
                // here instead of clipped there. The band's own rectangular clip
                // is what shortens the bar as the dwell runs out, so the draining
                // edge stays a straight cut while both bottom corners follow the
                // card.
                Item {
                    id: countdown

                    x: 0
                    y: card.height - countdown.height
                    height: 3
                    width: card.width * Math.max(0, Math.min(1, card.model.remainingFraction !== undefined
                                                                ? card.model.remainingFraction : 0))
                    visible: !card.standing
                    clip: true

                    Shape {
                        // Offset so the outline lands where the card actually
                        // is; the band clips everything above it away.
                        x: 0
                        y: -(card.height - countdown.height)
                        width: card.width
                        height: card.height

                        ShapePath {
                            fillColor: Qt.alpha(card.tone, 0.6)
                            strokeWidth: -1

                            PathRectangle {
                                width: card.width
                                height: card.height
                                radius: card.radius
                            }
                        }
                    }

                    // The model recomputes this ten times a second, which on a
                    // 372 px card is a visible step per update rather than a
                    // moving bar. Interpolating over exactly one tick makes it
                    // continuous: each new value arrives as the previous
                    // animation lands, and a late tick is absorbed instead of
                    // showing up as a jump.
                    Behavior on width {
                        NumberAnimation {
                            duration: 100
                            easing.type: Easing.Linear
                        }
                    }
                }
            }
        }
    }
}
