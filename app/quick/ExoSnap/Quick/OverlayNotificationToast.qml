pragma ComponentBehavior: Bound

import QtQuick

// Transient notification toasts, stacked bottom-right of the screen that hosts
// the ExoSnap window. Ported from app/ui/overlay/NotificationToastWindow.cpp.
//
// Input model: one translucent window spans the whole stack, so the transparent
// gaps between cards would swallow clicks meant for the app behind. Rather than
// Qt.WindowTransparentForInput — which would also kill the action buttons — the
// window keeps a mask covering only the card rectangles, rebuilt whenever the
// stack changes. That is why this overlay is only partially click-through.
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

    // Text on a tone-filled button. Deliberately not a theme token: it has to
    // stay legible on all four tone fills, which no single ink token does.
    readonly property color buttonInk: "#0E0E10"

    signal actionTriggered(int sequence, int action)
    signal dismissRequested(int sequence)

    // See OverlayRecording.qml: an inherited transient parent would take the
    // toasts down with the app window, and toasts about a finished recording are
    // most useful exactly when the app is minimised.
    transientParent: null

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus | Qt.WindowTransparentForInput
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

    function toneColor(tone) {
        return tone === "success" ? ExoTheme.success
             : tone === "caution" ? ExoTheme.warning
             : tone === "error" ? ExoTheme.error
             : ExoTheme.accent
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
            color: pill.primary ? root.buttonInk : ExoTheme.text
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

                readonly property color tone: root.toneColor(card.model.tone)
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
                        stroke: card.tone
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
                        wrapMode: Text.WordWrap
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

                Text {
                    id: dismiss

                    x: root.cardWidth - 15 - 18
                    y: 14
                    width: 18
                    height: 18
                    color: dismissArea.containsMouse ? ExoTheme.text : ExoTheme.textDim

                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Dismiss notification")
                    Accessible.onPressAction: root.dismissRequested(card.model.sequence)

                    MouseArea {
                        id: dismissArea

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.dismissRequested(card.model.sequence)
                    }
                }

                // Countdown hairline along the bottom edge of timed toasts.
                Rectangle {
                    height: 3
                    width: card.width * Math.max(0, Math.min(1, card.model.remainingFraction !== undefined
                                                                ? card.model.remainingFraction : 0))
                    anchors.bottom: parent.bottom
                    visible: !card.standing
                    color: Qt.alpha(card.tone, 0.6)
                }
            }
        }
    }
}
