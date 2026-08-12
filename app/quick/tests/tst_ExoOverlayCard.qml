import QtQuick
import QtQuick.Layouts
import QtTest

import ExoSnap.Quick.TestControls

// The two layout invariants the pre-0.9 review found broken on the crash
// consent surface at the 860x700 minimum window, asserted as geometry rather
// than judged on a screenshot: a screenshot shows that the card overlapped the
// title band, but nothing fails when it starts doing so again.
TestCase {
    id: testCase

    name: "ExoOverlayCard"
    when: windowShown
    width: 860
    height: 700
    visible: true

    Component {
        id: cardComponent

        ExoOverlayCard {
            title: "The previous session did not shut down normally"
            subtitle: "PROBLEM REPORT"
            hint: "A local crash dump is available and can help determine the cause."
            severity: "warning"

            // Far taller than the card can ever be, so the body is guaranteed
            // to scroll — the state an expanded disclosure produces.
            Rectangle {
                objectName: "tallBody"
                color: "transparent"
                implicitHeight: 4000
                Layout.fillWidth: true
            }

            persistent: [
                Rectangle {
                    objectName: "consentStrip"
                    color: "transparent"
                    implicitHeight: 20
                    Layout.fillWidth: true
                }
            ]

            actions: [
                Item {
                    Layout.fillWidth: true
                },
                Rectangle {
                    objectName: "commitAction"
                    color: "transparent"
                    implicitWidth: 120
                    implicitHeight: 36
                }
            ]
        }
    }

    Component {
        id: plainCardComponent

        ExoOverlayCard {
            title: "Nothing standing to decide"

            Rectangle {
                objectName: "shortBody"
                color: "transparent"
                implicitHeight: 40
                Layout.fillWidth: true
            }
        }
    }

    // A shell with live background controls under an active modal card — the
    // arrangement AppShell produces, reduced to what the focus chain can see.
    Component {
        id: focusShellComponent

        Item {
            Column {
                anchors.fill: parent

                ExoButton {
                    objectName: "backgroundA"
                    text: "Background A"
                }

                ExoButton {
                    objectName: "backgroundB"
                    text: "Background B"
                }

                ExoButton {
                    objectName: "backgroundC"
                    text: "Background C"
                }
            }

            ExoOverlayCard {
                objectName: "focusOverlay"
                anchors.fill: parent
                title: "A decision that must be answered here"
                subtitle: "PROBLEM REPORT"
                severity: "warning"
                focus: true

                Rectangle {
                    objectName: "focusBody"
                    color: "transparent"
                    implicitHeight: 40
                    Layout.fillWidth: true
                }

                actions: [
                    ExoButton {
                        objectName: "overlayA"
                        text: "Overlay A"
                    },
                    Item {
                        Layout.fillWidth: true
                    },
                    ExoButton {
                        objectName: "overlayB"
                        text: "Overlay B"
                    },
                    ExoButton {
                        objectName: "overlayC"
                        text: "Overlay C"
                        tone: "primary"
                    }
                ]
            }
        }
    }

    function makeCard(component, w, h, topInset) {
        let card = createTemporaryObject(component, testCase, {
            width: w,
            height: h,
            contentTopInset: topInset
        });
        verify(card);
        waitForRendering(card);
        return card;
    }

    function cardRect(card) {
        // The card is the one child Rectangle that carries the surface; find it
        // by the item that is neither the scrim (full-bleed) nor a MouseArea.
        for (let i = 0; i < card.children.length; ++i) {
            let child = card.children[i];
            if (child instanceof Rectangle && child.height < card.height)
                return child;
        }
        return null;
    }

    function find(card, objectName) {
        let stack = [card];
        while (stack.length > 0) {
            let item = stack.pop();
            if (item.objectName === objectName)
                return item;
            for (let i = 0; i < item.children.length; ++i)
                stack.push(item.children[i]);
        }
        return null;
    }

    function test_card_stays_below_the_shell_chrome() {
        let card = makeCard(cardComponent, 860, 700, 40);
        let surface = cardRect(card);
        verify(surface);

        // The scrim still covers the whole shell — only the card is inset.
        verify(surface.y >= 40, "card top " + surface.y + " overlaps the 40 px title band");
        verify(surface.y + surface.height <= card.height, "card bottom leaves the window");
    }

    function test_no_inset_keeps_the_card_centred_in_the_whole_item() {
        let card = makeCard(cardComponent, 860, 700, 0);
        let surface = cardRect(card);
        verify(surface);
        // Symmetric within a pixel: with no chrome to avoid, the card centres on
        // the item exactly as it always did.
        let top = surface.y;
        let bottom = card.height - (surface.y + surface.height);
        verify(Math.abs(top - bottom) <= 1, "top " + top + " vs bottom " + bottom);
    }

    function test_consent_and_actions_stay_visible_while_the_body_scrolls() {
        let card = makeCard(cardComponent, 860, 700, 40);
        let surface = cardRect(card);
        verify(surface);

        let consent = find(card, "consentStrip");
        let action = find(card, "commitAction");
        verify(consent);
        verify(action);

        // Both are measured against the CARD, because the card clips: an item
        // inside the scrolled body can be positioned correctly in its own
        // column and still be nowhere on screen.
        let consentRect = consent.mapToItem(surface, 0, 0);
        verify(consentRect.y >= 0 && consentRect.y + consent.height <= surface.height,
               "consent strip is outside the card: y=" + consentRect.y);

        let actionRect = action.mapToItem(surface, 0, 0);
        verify(actionRect.y >= 0 && actionRect.y + action.height <= surface.height,
               "action row is outside the card: y=" + actionRect.y);

        // The decision sits above the actions that commit it.
        verify(consentRect.y + consent.height <= actionRect.y);
    }

    function test_a_card_without_a_standing_decision_grows_no_extra_strip() {
        let withStrip = makeCard(cardComponent, 860, 700, 40);
        let without = makeCard(plainCardComponent, 860, 700, 40);
        verify(cardRect(withStrip));
        verify(cardRect(without));
        // Recovery and the recording-error surface pass no `persistent` content,
        // so neither the strip nor its divider may appear for them.
        compare(find(without, "consentStrip"), null);
    }

    // The card is a modal layer inside the application, not a window inside a
    // window: it names the surface, and it does not redraw the shell's chrome
    // 40 px below the real one.
    function test_card_names_the_surface_without_imitating_a_window() {
        let card = makeCard(cardComponent, 860, 700, 40);
        let texts = collectText(card);
        verify(texts.indexOf("PROBLEM REPORT") !== -1, "the eyebrow must name the surface: " + texts.join(" | "));
        compare(texts.indexOf("exo"), -1, "a second wordmark inside the card");
        compare(texts.indexOf("snap"), -1, "a second wordmark inside the card");
    }

    function collectText(item) {
        let out = [];
        let stack = [item];
        while (stack.length > 0) {
            let current = stack.pop();
            if (current.text !== undefined && typeof current.text === "string" && current.text !== "")
                out.push(current.text);
            for (let i = 0; i < current.children.length; ++i)
                stack.push(current.children[i]);
        }
        return out;
    }

    function isInside(ancestor, item) {
        let current = item;
        while (current) {
            if (current === ancestor)
                return true;
            current = current.parent;
        }
        return false;
    }

    // Blocking the pointer is only half of modality. A card that swallows every
    // click but lets Tab walk into the page behind it leaves a keyboard user
    // pressing Return on a Record control while a consent question is still on
    // screen — so the ring the overlay owns has to close on itself.
    function test_keyboard_focus_cannot_leave_the_active_overlay() {
        let shell = createTemporaryObject(focusShellComponent, testCase, { width: 860, height: 700 });
        verify(shell);
        waitForRendering(shell);

        let overlay = find(shell, "focusOverlay");
        let first = find(shell, "overlayA");
        verify(overlay);
        verify(first);

        first.forceActiveFocus();
        verify(first.activeFocus);

        // Enough steps to walk the overlay's own ring several times over: an
        // escape that only happens on the fourth Tab is still an escape.
        for (let i = 0; i < 8; ++i) {
            keyClick(Qt.Key_Tab);
            let focused = shell.Window.activeFocusItem;
            verify(focused, "focus vanished entirely after Tab #" + (i + 1));
            verify(isInside(overlay, focused),
                   "Tab #" + (i + 1) + " left the modal overlay for '" + focused.objectName + "'");
        }

        for (let i = 0; i < 8; ++i) {
            keyClick(Qt.Key_Backtab, Qt.ShiftModifier);
            let focused = shell.Window.activeFocusItem;
            verify(focused, "focus vanished entirely after Shift+Tab #" + (i + 1));
            verify(isInside(overlay, focused),
                   "Shift+Tab #" + (i + 1) + " left the modal overlay for '" + focused.objectName + "'");
        }
    }

    // The card parks focus inside itself when it opens — but on nothing that
    // commits anything. Both halves matter: parking outside leaves Shift+Tab an
    // exit, and parking on an action answers a consent question with a stray
    // Return. Escape must still reach the card from wherever focus was parked.
    function test_open_card_holds_focus_without_pre_focusing_an_action() {
        let shell = createTemporaryObject(focusShellComponent, testCase, { width: 860, height: 700 });
        verify(shell);
        waitForRendering(shell);

        let overlay = find(shell, "focusOverlay");
        verify(overlay);

        let focused = shell.Window.activeFocusItem;
        verify(focused, "the open card holds no focus at all");
        verify(isInside(overlay, focused),
               "focus parked outside the modal on '" + focused.objectName + "'");

        for (const name of ["overlayA", "overlayB", "overlayC"]) {
            let action = find(shell, name);
            verify(action);
            verify(!action.activeFocus, name + " is pre-focused; a stray Return would press it");
        }

        let dismissals = signalSpy.createObject(testCase, { target: overlay, signalName: "dismissed" });
        keyClick(Qt.Key_Escape);
        compare(dismissals.count, 1, "Escape did not reach the card from the parked focus");
    }

    Component {
        id: signalSpy

        SignalSpy {}
    }

    function test_card_exposes_dialog_semantics() {
        let card = makeCard(cardComponent, 860, 700, 40);
        let surface = cardRect(card);
        verify(surface);
        compare(surface.Accessible.role, Accessible.Dialog);
        compare(surface.Accessible.name, "The previous session did not shut down normally");
    }
}
