import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The shared in-window overlay: scrim + centred card with a severity hero, an
// eyebrow naming the surface, a title, an optional hint, a scrollable body and
// an action row.
//
// Deliberately NOT a window: no chrome bar, no second wordmark, no imitation
// title strip. This is a modal layer inside an application whose own title band
// is 40 px above it, and a card that draws its own frame reads as a detached
// window that happens to be inside another one. What that bar carried worth
// keeping — the name of the surface — is now the eyebrow over the heading.
//
// Three surfaces carry exactly this shape in the Widgets product — RecoveryOverlay,
// RecordingErrorOverlay and CrashReportOverlay — and each rebuilt it by hand from
// inline QSS. They are consolidated here because the semantics genuinely match,
// not because the pixels happen to: an app-modal, in-window, non-OS surface that
// reports something the user must resolve before continuing.
//
// What it deliberately does NOT own: which actions exist, what they do, or when
// the surface appears. Those belong to the calling surface and, above it, to C++.
//
// Responsive by construction: the card is capped rather than fixed, so at the
// 860x700 minimum window it uses the available width minus a margin, and its body
// scrolls instead of pushing the action row off the bottom.
//
// Three regions, not two: chrome/title and the actions are fixed, the body
// scrolls, and between them sits an optional `persistent` strip for anything the
// user must SEE while deciding rather than merely be able to scroll to. The
// scrim covers the whole item; `contentTopInset` keeps the card itself out of
// the shell's title band.
Item {
    id: root

    required property string title
    // Names the surface, drawn as the mono eyebrow above the title. Callers
    // pass it in the eyebrow's own casing, as every other eyebrow in the
    // product does, rather than having the component upper-case a translated
    // string on their behalf.
    property string subtitle: ""
    property string hint: ""
    // "none" | "info" | "warning" | "error" — how serious this surface is, drawn
    // as the hero block left of the title. "none" keeps the plain heading, which
    // is what a surface that merely asks a question wants.
    property string severity: "none"

    readonly property color _severityColor: root.severity === "error" ? ExoTheme.error
                                          : root.severity === "warning" ? ExoTheme.warning
                                          : ExoTheme.accent
    // Escape and a backdrop click both mean "leave this for now". Surfaces where
    // that is not a safe answer (an unrecoverable recording error the user has to
    // acknowledge) set this false and offer an explicit action instead.
    property bool dismissOnEscape: true
    property int maxCardWidth: 620

    // Height at the top of this item that the CARD must stay out of, while the
    // scrim still covers it. The shell's 40 px title band is the only such
    // region today: the scrim has to reach it (a shell that stays lit behind a
    // modal reads as still usable), but a card centred in the whole window
    // overlapped the brand, the navigation and the window buttons at the 860x700
    // minimum. Two different rectangles, so two different values.
    property int contentTopInset: 0

    // The scrollable body between hint and actions — the default slot, because
    // that is what a caller writes most of.
    default property alias body: bodyColumn.data
    // Content that must stay VISIBLE while the body scrolls: a consent tick, a
    // "don't ask again" choice — anything the user is being asked to decide,
    // as opposed to the material they are deciding on. Scrolling it away with
    // the body leaves the action row asking a question whose terms are off
    // screen.
    property alias persistent: persistentColumn.data
    // The action row. Fill it with ExoButtons; the card supplies alignment.
    property alias actions: actionRow.data
    // For a surface whose standing decision exists in only SOME of its modes —
    // the "What's new" suppress tick is post-update only. Set false and the strip
    // and its divider go with it, rather than reserving an empty band and a rule
    // for a control that is not there. Said explicitly instead of derived from
    // child visibility: a Layout that is itself invisible stops being laid out,
    // so its implicit height is not a reliable answer to "is anything in here".
    property bool persistentVisible: true

    signal dismissed()

    // Where the keyboard was before this surface took it. Published rather than
    // private because restoring it is the SHELL's job: the loader that unloads
    // this card outlives it, and a focus assignment made from a dying item's own
    // destruction handler is immediately undone by the focus scope coming down
    // around it.
    //
    // Null when there was no focus to take (a surface raised during startup, like
    // the post-update "What's new" auto-show) or when the outgoing item is already
    // inside this card — neither is something to return to.
    readonly property Item focusReturnItem: root._focusReturn
    property Item _focusReturn: null

    // Blocking the pointer is only half of modality, and it was the only half
    // this card had. Qt Quick's focus chain walks the whole scene graph and
    // stops at nothing but an invisible or disabled subtree, so Tab off the last
    // action landed on the Record page behind the scrim while the card was still
    // asking its question — a keyboard user could press Return on a control the
    // mouse could not even reach.
    //
    // Two zero-sized sentinels bracket the card's own chain and fold that step
    // back inside. Deliberately not a QQuickPopup: these surfaces are in-window
    // layers by construction (the shell keeps its own title band above them, and
    // ADR 0022's editor shares the region), so swapping the component to inherit
    // one behaviour would change what they ARE. Deliberately not `enabled: false`
    // on the shell either — that would paint the whole application behind the
    // scrim in its disabled rung.
    Component.onCompleted: {
        // Read FIRST, before this card parks focus on its own head — one line
        // later the answer is `focusHead`.
        const outgoing = root.Window.activeFocusItem;
        root._focusReturn = outgoing !== null && !root._contains(outgoing) ? outgoing : null;
        // Parked on the head rather than on an action: pre-focusing either half
        // of a consent question answers it on the user's behalf at the first
        // stray Return.
        focusHead.forceActiveFocus(Qt.OtherFocusReason);
        focusHead.armed = true;
    }

    function _contains(item: Item): bool {
        let current = item;
        while (current !== null) {
            if (current === root)
                return true;
            current = current.parent;
        }
        return false;
    }

    // Blocks every click that would otherwise reach the page underneath: the
    // surfaces using this card all report state the user must resolve, so acting
    // on the shell behind them is never the intent.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onClicked: {
            if (root.dismissOnEscape)
                root.dismissed();
        }
    }

    Rectangle {
        anchors.fill: parent
        color: ExoTheme.overlayScrim
    }

    Keys.onEscapePressed: function (event) {
        if (root.dismissOnEscape) {
            root.dismissed();
            event.accepted = true;
        }
    }

    Rectangle {
        id: card

        // Centred in the USABLE content region rather than in the whole item:
        // the scrim covers the shell's title band, the card does not sit on it.
        readonly property int availableHeight: root.height - root.contentTopInset

        // Swallows clicks so the backdrop MouseArea above cannot dismiss the
        // surface when the user simply clicks inside the card.
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.contentTopInset + Math.max(0, (card.availableHeight - card.height) / 2)
        width: Math.min(root.maxCardWidth, root.width - 2 * ExoTheme.spacingXl)
        height: Math.min(layout.implicitHeight, card.availableHeight - 2 * ExoTheme.spacingXl)
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusLg
        clip: true

        // An interruption surface, and it says so: assistive tools treat a
        // Dialog as the thing to read out and stay inside, rather than as one
        // more panel on the page behind it.
        Accessible.role: Accessible.Dialog
        Accessible.name: root.title
        Accessible.description: root.hint

        // FIRST child on purpose: the focus chain follows the item tree, so this
        // is the step immediately before everything the card owns. Reaching it
        // means Shift+Tab has walked off the front — except on the very first
        // frame, when the card parks its own focus here and nothing has moved
        // yet, which is what `armed` distinguishes.
        Item {
            id: focusHead

            property bool armed: false

            activeFocusOnTab: true
            width: 0
            height: 0
            onActiveFocusChanged: {
                if (!focusHead.activeFocus || !focusHead.armed)
                    return;

                const last = focusTail.nextItemInFocusChain(false);
                if (last && last !== focusHead)
                    last.forceActiveFocus(Qt.BacktabFocusReason);
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }

        ColumnLayout {
            id: layout

            // Inset by the border, so the card's own hairline is never covered
            // by a child painted to the edge. Qt Quick's `clip` is RECTANGULAR
            // -- it clips to the bounding box and never to `radius` -- so a
            // full-bleed child at the top of a rounded card paints square
            // corners over the two the card just rounded.
            anchors.fill: parent
            anchors.margins: card.border.width
            spacing: 0

            // ---- Severity hero + title + hint ----
            //
            // The hero is what tells the user, before they read a word, whether
            // this surface is a failure, a caution or a routine question. Without
            // it every one of these surfaces was a card with a heading in it, and
            // "Recording could not start" carried exactly as much visual weight as
            // a settings section title.
            RowLayout {
                spacing: ExoTheme.spacingLg
                Layout.fillWidth: true
                Layout.leftMargin: ExoTheme.spacingXl
                Layout.rightMargin: ExoTheme.spacingXl
                Layout.topMargin: ExoTheme.spacingXl

                Rectangle {
                    color: root.severity === "error" ? ExoTheme.errorSurface
                         : root.severity === "warning" ? ExoTheme.warningSurface : ExoTheme.surfaceRaised
                    border.width: 1
                    border.color: root._severityColor
                    radius: ExoTheme.radiusMd
                    visible: root.severity !== "none"
                    Layout.preferredWidth: 44
                    Layout.preferredHeight: 44
                    Layout.alignment: Qt.AlignTop

                    ExoGlyph {
                        anchors.centerIn: parent
                        kind: root.severity === "error" ? ExoGlyph.Close
                              : root.severity === "warning" ? ExoGlyph.Warning : ExoGlyph.Info
                        color: root._severityColor
                        strokeWidth: 2
                        width: 22
                        height: 22
                    }
                }

                ColumnLayout {
                    spacing: ExoTheme.spacingXs
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter

                    // What this surface IS, as a mono kicker over its heading —
                    // the same rung every other eyebrow in the product uses.
                    // It used to sit in a title bar of its own, next to a second
                    // copy of the wordmark: a card drawn like a standalone
                    // window, floating inside a window that already carries the
                    // brand 40 px above it. Naming the surface is worth one
                    // line; imitating an application frame is not.
                    Label {
                        text: root.subtitle
                        textFormat: Text.PlainText
                        visible: root.subtitle !== ""
                        color: ExoTheme.textDim
                        Layout.fillWidth: true
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: ExoTheme.fontEyebrow
                            letterSpacing: 0.6
                        }
                    }

                    Label {
                        text: root.title
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: ExoTheme.text
                        Layout.fillWidth: true
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontPageTitle
                            weight: Font.DemiBold
                        }
                    }

                    Label {
                        text: root.hint
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        visible: root.hint !== ""
                        color: ExoTheme.textSecondary
                        Layout.fillWidth: true
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontSecondary
                        }
                    }
                }
            }

            // ---- Body ----
            // fillHeight rather than a fixed height: the card is capped at the
            // window height, so whatever is left after chrome/title/actions is
            // what the body gets, and it scrolls inside that.
            ExoScrollView {
                id: bodyScroll

                // Word-wrapped content inside a width-capped card: the content
                // width feeds its own height, so the gutters are reserved
                // unconditionally to cut that cycle (see ExoScrollView's note).
                reserveScrollBarGutters: true
                contentWidth: availableWidth
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: ExoTheme.spacingLg
                Layout.bottomMargin: ExoTheme.spacingLg
                Layout.leftMargin: ExoTheme.spacingXl
                Layout.rightMargin: ExoTheme.spacingXl

                ColumnLayout {
                    id: bodyColumn

                    spacing: ExoTheme.spacingMd
                    width: bodyScroll.availableWidth
                }
            }

            // ---- Persistent decision strip ----
            //
            // Outside the ScrollView on purpose. At the 860x700 minimum window
            // the crash surface's expanded report contents pushed its consent
            // tick out of the scroll viewport while the buttons stayed put, so
            // the user could be one click from persisting a choice whose control
            // was off screen. What is being decided may scroll; the decision
            // itself may not.
            ColumnLayout {
                id: persistentColumn

                spacing: ExoTheme.spacingXs
                visible: root.persistentVisible && persistentColumn.children.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: ExoTheme.spacingXl
                Layout.rightMargin: ExoTheme.spacingXl
                Layout.bottomMargin: persistentColumn.visible ? ExoTheme.spacingLg : 0
            }

            // Only drawn when there IS a persistent strip: it separates the
            // standing decision from the actions that commit it. A card without
            // one keeps the plain body-to-actions gap it has today.
            Rectangle {
                color: ExoTheme.line
                visible: persistentColumn.visible
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                Layout.bottomMargin: ExoTheme.spacingLg
            }

            // ---- Actions ----
            RowLayout {
                id: actionRow

                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true
                Layout.leftMargin: ExoTheme.spacingXl
                Layout.rightMargin: ExoTheme.spacingXl
                Layout.bottomMargin: ExoTheme.spacingLg
            }
        }

        // LAST child, mirroring the head: reaching it means Tab has walked off
        // the end of the card, so focus folds around to the first control the
        // card owns instead of onto the shell behind the scrim.
        Item {
            id: focusTail

            activeFocusOnTab: true
            width: 0
            height: 0
            onActiveFocusChanged: {
                if (!focusTail.activeFocus)
                    return;

                const first = focusHead.nextItemInFocusChain(true);
                if (first && first !== focusTail)
                    first.forceActiveFocus(Qt.TabFocusReason);
                else
                    // A card with no focusable content at all still may not hand
                    // the shell behind it a keyboard entry point.
                    focusHead.forceActiveFocus(Qt.TabFocusReason);
            }
        }
    }
}
