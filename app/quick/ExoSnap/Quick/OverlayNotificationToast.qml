pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
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
//  - the card resolves its colours from the WINDOWS SHELL's own appearance
//    (`ExoTheme.shell*`), not the fixed-dark `overlay*` family the other four
//    capture-excluded overlays use and not the application's own appearance --
//    it sits on the desktop beside Windows' own notifications, not over
//    recorded content;
//  - a dismiss ✕ appears only on hover (or keyboard focus, where reachable --
//    see the Escape Shortcut below for why that is a real constraint here);
//  - with exactly one action the whole card is clickable, with no marker glyph
//    -- the affordance is the pointer cursor and the ground stepping to its
//    hover rung;
//  - with two actions each gets its own plain-text button;
//  - the body wraps up to three lines and ellipsizes beyond that -- the hub,
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
    readonly property int cardWidth: 340
    // The window is wider than the card on every side so the soft shadow's
    // penumbra has room; the cards sit inset by this margin.
    readonly property int shadowMargin: 20
    readonly property int stackGap: 12
    readonly property int cardPadding: 12

    // ── The card's vertical rhythm, derived rather than eyeballed ────────────
    readonly property int titleSize: 14
    readonly property int titleLineHeight: 19
    readonly property int titleCapHeight: 10
    readonly property int glyphSize: 14
    // Half-leading ((titleLineHeight - titleSize) / 2) plus an ascent of about
    // 0.75em puts the baseline below the content box's top edge; the cap top
    // sits titleCapHeight above that baseline. Adding the card's own padding
    // and its 1px border gives the glyph's y from the card's outer edge, so a
    // future change to the title's size or line box carries the glyph with it
    // instead of leaving it to be re-eyeballed by hand.
    readonly property int glyphCapTop: 1 + root.cardPadding
                                        + Math.round((root.titleLineHeight - root.titleSize) / 2
                                                      + 0.75 * root.titleSize - root.titleCapHeight)

    // Two-layer elevation (see the card delegate below): a wide soft penumbra
    // that lifts the card, and a tight contact shadow that gives it an edge.
    // MultiEffect has no literal box-shadow equivalent, so blurMax/shadowBlur
    // approximate the design's blur radius rather than reproduce it exactly.
    readonly property color shadowWideColor: ExoTheme.shellDark ? Qt.rgba(0, 0, 0, 0.35)
                                                                 : Qt.rgba(20 / 255, 26 / 255, 38 / 255, 0.14)
    readonly property color shadowTightColor: ExoTheme.shellDark ? Qt.rgba(0, 0, 0, 0.25)
                                                                  : Qt.rgba(20 / 255, 26 / 255, 38 / 255, 0.08)

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

    // Named rather than left to Qt's default: an untitled QWindow inherits the
    // application display name, and five overlays titled "ExoSnap" made the main
    // window impossible to identify by owner pid and title. See OverlayRecording.
    title: qsTr("ExoSnap Overlay — Notification")
    color: "transparent"

    visible: exclusion.granted && stack.count > 0

    width: root.cardWidth + 2 * root.shadowMargin
    height: stack.contentHeight + 2 * root.shadowMargin
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
        for (let i = 0; i < stack.count; ++i) {
            const card = stack.itemAtIndex(i)
            if (!card)
                continue
            rects.push(Qt.rect(stack.x + card.x - root.shadowMargin,
                               stack.y + card.y - 10,
                               card.width + 2 * root.shadowMargin,
                               card.height + 48))
        }
        exclusion.setClickThroughRegion(rects)
    }

    // Escape dismisses the toast nearest the anchor -- the LAST entry, which
    // NotificationManager::Enqueue already keeps as the timed toast when one is
    // showing, else the newest standing one, so "last" and "most prominent" are
    // the same card by construction.
    //
    // An ApplicationShortcut rather than a per-card Keys handler: this window
    // carries Qt.WindowDoesNotAcceptFocus (above) and can never hold real OS
    // keyboard focus, so nothing inside it would ever see a key event through
    // the ordinary window-focus route. Qt.ApplicationShortcut instead fires
    // whenever any window of THIS application -- in practice, the main window --
    // is the focused one, which does not require this window to be.
    function dismissFocusedToast() {
        if (stack.count <= 0)
            return
        const last = stack.itemAtIndex(stack.count - 1)
        if (last)
            root.dismissRequested(last.model.sequence)
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        enabled: root.visible
        onActivated: root.dismissFocusedToast()
    }

    component ActionLabel: Item {
        id: actionLabel

        property string label: ""
        property color ink: ExoTheme.shellAccent
        property int weight: Font.DemiBold

        signal activated()

        implicitWidth: labelText.implicitWidth
        // 5px of padding above and below a 14px line box: an invisible 24px
        // hit target under a label that draws no background of its own.
        implicitHeight: 24

        Accessible.role: Accessible.Button
        Accessible.name: actionLabel.label
        Accessible.onPressAction: actionLabel.activated()

        Text {
            id: labelText

            anchors.centerIn: parent
            text: actionLabel.label
            textFormat: Text.PlainText
            color: actionLabel.ink
            font {
                family: ExoTheme.sansFamily
                pixelSize: 13
                weight: actionLabel.weight
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: actionLabel.activated()
        }
    }

    ListView {
        id: stack

        x: root.shadowMargin
        y: root.shadowMargin
        width: root.cardWidth
        height: contentHeight
        spacing: root.stackGap
        interactive: false
        boundsBehavior: Flickable.StopAtBounds
        model: root.toasts

        onContentHeightChanged: root.rebuildMask()
        onCountChanged: root.rebuildMask()

        // The other cards sliding into the gap a dismissed one leaves behind --
        // the stack-reflow half of the motion contract. `displaced` needs no
        // explicit from/to: the view already knows each item's before and
        // after position and animates between them.
        removeDisplaced: Transition {
            NumberAnimation {
                properties: "y"
                duration: 160
                easing.type: Easing.OutCubic
            }
        }

        delegate: Item {
            id: cardRoot

            required property var model

            width: card.width
            height: card.height

            // The wide, soft penumbra. Applied to this OUTER item so it
            // shadows the inner card's already-composited (card + tight
            // shadow) texture, giving the two layers their own visible depth
            // rather than one blur pass standing in for both.
            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: root.shadowWideColor
                shadowVerticalOffset: 8
                shadowBlur: 1.0
                blurMax: 24
            }

            // Resting state is fully opaque with no offset -- a delegate that
            // is never added dynamically (the initial population of an
            // already-populated model) must still render normally rather than
            // depend on ListView.onAdd firing to reach opacity 1. Enter (below)
            // explicitly forces the "before" state and animates out of it,
            // rather than making that the resting default.
            opacity: 1
            // A Translate rather than animating `y` directly for the enter
            // slide, because `y` is what the ListView itself sets to lay the
            // stack out -- animating it here would fight that positioning
            // instead of offsetting it.
            transform: [
                Translate { id: enterShift },
                Translate { id: leaveShift }
            ]

            // Named rather than assigned inline to ListView.onAdd/onRemove:
            // assigning an animation object directly as a signal handler's
            // value is deprecated (Qt warns on it at runtime), so these are
            // declared once here and started from the handler instead.
            SequentialAnimation {
                id: enterAnimation

                PropertyAction { target: cardRoot; property: "opacity"; value: 0 }
                PropertyAction { target: enterShift; property: "y"; value: 8 }
                ParallelAnimation {
                    NumberAnimation { target: cardRoot; property: "opacity"; to: 1; duration: 160; easing.type: Easing.OutCubic }
                    NumberAnimation { target: enterShift; property: "y"; to: 0; duration: 160; easing.type: Easing.OutCubic }
                }
            }

            SequentialAnimation {
                id: leaveAnimation

                PropertyAction { target: cardRoot; property: "ListView.delayRemove"; value: true }
                ParallelAnimation {
                    NumberAnimation { target: cardRoot; property: "opacity"; to: 0; duration: 120; easing.type: Easing.InCubic }
                    NumberAnimation { target: leaveShift; property: "x"; to: 12; duration: 120; easing.type: Easing.InCubic }
                }
                PropertyAction { target: cardRoot; property: "ListView.delayRemove"; value: false }
            }

            ListView.onAdd: enterAnimation.start()
            ListView.onRemove: leaveAnimation.start()

            Rectangle {
                id: card

                // Aliases `cardRoot.model` rather than declaring its own
                // `required property`: under ComponentBehavior: Bound, that
                // property belongs on the delegate ROOT (`cardRoot`), and every
                // reference below keeps reading `card.model` unchanged.
                property var model: cardRoot.model

                readonly property color tone: ExoTheme.shellAdvisoryTone(card.model.tone)
                readonly property int actionCount: card.model.actionCount !== undefined ? card.model.actionCount : 0
                // One action means the card itself is the action; two get
                // their own plain-text buttons. There is no marker glyph for
                // the single-action case -- the pointer cursor and the hover
                // ground (below) are the whole affordance.
                readonly property bool cardIsAction: card.actionCount === 1
                // The countdown bar appears exactly when the toast leaves on its
                // own — a standing toast reports a condition that still holds.
                readonly property bool standing: card.model.standing === true
                readonly property bool hovered: cardHover.hovered

                width: root.cardWidth
                height: 2 * card.border.width + 2 * root.cardPadding + textBlock.height
                        + (actionRow.visible ? 5 + actionRow.height : 0)
                radius: ExoTheme.radiusMd
                color: card.cardIsAction && card.hovered ? ExoTheme.shellSurfaceHover : ExoTheme.shellSurfaceRaised
                border.width: 1
                border.color: ExoTheme.shellLine

                // The tight contact shadow, the layer nearer the card. See
                // `cardRoot`'s own layer above for the wide one.
                layer.enabled: true
                layer.effect: MultiEffect {
                    shadowEnabled: true
                    shadowColor: root.shadowTightColor
                    shadowVerticalOffset: 1
                    shadowBlur: 0.6
                    blurMax: 4
                }

                HoverHandler {
                    id: cardHover
                }

                Accessible.role: Accessible.AlertMessage
                Accessible.name: card.model.title
                Accessible.description: card.model.body

                // The severity glyph, bare rather than inside a tinted,
                // outlined box: the tone is carried by the glyph's own colour,
                // which is what the notification hub does with the same four
                // shapes. 14x14 (down from an earlier 16x16), its top aligned
                // to the title's CAP height rather than its line box -- a box
                // aligned glyph reads as sitting slightly too low next to a
                // capital letter's actual top.
                ExoGlyph {
                    id: severityGlyph

                    x: 19
                    y: root.glyphCapTop
                    width: root.glyphSize
                    height: root.glyphSize
                    kind: card.model.tone === "success" ? ExoGlyph.Check
                        : card.model.tone === "caution" ? ExoGlyph.Warning
                        : card.model.tone === "error" ? ExoGlyph.Close
                        : ExoGlyph.Info
                    color: card.tone
                }

                Column {
                    id: textBlock

                    x: 44
                    y: root.cardPadding
                    // Clear the ✕ column on the right; the chevron column it
                    // used to also clear is gone along with the chevron.
                    width: root.cardWidth - 2 * card.border.width - 44 - (15 + 18 + 6)
                    spacing: 3

                    Text {
                        width: parent.width
                        text: card.model.title
                        textFormat: Text.PlainText
                        color: ExoTheme.shellInk
                        elide: Text.ElideRight
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: root.titleSize
                            weight: Font.DemiBold
                        }
                    }

                    Text {
                        objectName: "toastBody"
                        width: parent.width
                        visible: text.length > 0
                        text: card.model.body !== undefined ? card.model.body : ""
                        textFormat: Text.PlainText
                        color: ExoTheme.shellInkSecondary
                        // WrapAnywhere as the fallback, not WordWrap alone: a
                        // file path is one unbreakable token, and WordWrap has
                        // no legal break in it -- so the line simply grew past
                        // the card and out of the window, with elide never
                        // reached because eliding only applies to the last of
                        // several lines. Any body that cannot wrap now breaks
                        // mid-token instead of overrunning.
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        // Grow-to-fit up to three lines; beyond that the last
                        // line elides. The notification hub always keeps the
                        // full text.
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        lineHeight: 1.4
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: 13
                        }
                    }
                }

                Row {
                    id: actionRow

                    x: 44
                    y: textBlock.y + textBlock.height + 5
                    spacing: 16
                    visible: card.actionCount >= 2

                    ActionLabel {
                        label: card.model.primaryLabel !== undefined ? card.model.primaryLabel : ""
                        ink: ExoTheme.shellAccent
                        weight: Font.DemiBold
                        onActivated: root.actionTriggered(card.model.sequence, card.model.primaryAction)
                    }

                    ActionLabel {
                        label: card.model.secondaryLabel !== undefined ? card.model.secondaryLabel : ""
                        ink: ExoTheme.shellInkSecondary
                        weight: Font.Medium
                        onActivated: root.actionTriggered(card.model.sequence, card.model.secondaryAction)
                    }
                }

                // Single-action affordance: the whole card is the target. No
                // marker glyph -- the pointer cursor and `card.color`'s own
                // hover step (above) are the whole affordance.
                MouseArea {
                    anchors.fill: parent
                    enabled: card.cardIsAction
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.actionTriggered(card.model.sequence, card.model.primaryAction)
                }

                // The dismiss affordance. Empty at rest -- dismissal is chrome,
                // not an action, and a quiet card should stay quiet -- and
                // drawn only while the card is hovered. The 18px target keeps
                // its fixed position either way, so it never has to be found
                // in a new place the moment it appears.
                //
                // "...or keyboard focus" per the design is not reachable here:
                // this window can never hold real OS focus (see the Escape
                // Shortcut above), so hover is the only signal this surface
                // can actually observe.
                Item {
                    id: dismiss

                    objectName: "toastDismiss"
                    x: root.cardWidth - 15 - 18
                    y: root.cardPadding
                    width: 18
                    height: 18
                    visible: card.hovered

                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Dismiss notification")
                    Accessible.onPressAction: root.dismissRequested(card.model.sequence)

                    // A round ground one rung up, only while the pointer is on
                    // the glyph itself -- what makes an 18px target feel like
                    // a 24px one without spending 24px of layout on it.
                    Rectangle {
                        anchors.centerIn: parent
                        width: 24
                        height: 24
                        radius: 12
                        color: ExoTheme.shellSurfaceHover
                        visible: dismissArea.containsMouse
                    }

                    ExoGlyph {
                        anchors.centerIn: parent
                        width: 12
                        height: 12
                        kind: ExoGlyph.Close
                        color: dismissArea.containsMouse ? ExoTheme.shellInk : ExoTheme.shellInkDim
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
                // radius -- while the card's bottom arc cuts inwards over
                // exactly those rows. The bar's square ends therefore hung
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
                    // A hairline, and dimmed: at 3 px in the tone at 0.6 it
                    // was the brightest thing on the card and read as a
                    // progress bar for work in flight rather than as the dwell
                    // quietly running out.
                    height: 2
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
                            fillColor: Qt.alpha(card.tone, 0.35)
                            strokeWidth: -1

                            PathRectangle {
                                width: card.width
                                height: card.height
                                radius: card.radius
                            }
                        }
                    }

                    // The model recomputes this ten times a second, which on a
                    // 340 px card is a visible step per update rather than a
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
