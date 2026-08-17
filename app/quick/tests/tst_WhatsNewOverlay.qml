pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The release-notes surface, whose whole QML half went missing in the Qt Quick
// cutover: the backend kept writing the pending payload and the Settings card
// kept showing its link, but there was no document to open and no connection to
// open it — so the link did nothing and the post-update overlay never appeared on
// any machine.
//
// What is asserted here is behaviour the harness cannot see: which mode offers the
// suppress tick, what the primary action commits, that the tick reports the value
// that gets persisted, and that closing hands the keyboard back to the control the
// user opened this from. The document's appearance is judged with --visual-test
// (`whats-new`, `whats-new-post-update`).
TestCase {
    id: testCase

    name: "WhatsNewOverlay"
    when: windowShown
    width: 860
    height: 700
    visible: true

    // The shell arrangement AppShell produces, reduced to what these contracts can
    // see: a page carrying the Settings card's link, and the overlay on a Loader
    // driven by the adapter's `active` — with the focus hand-back on the LOADER,
    // because the loader outlives the card it unloads.
    Component {
        id: shellComponent

        Item {
            id: shell

            required property WhatsNewAdapter whatsNew

            Column {
                anchors.fill: parent

                ExoButton {
                    objectName: "cardLink"
                    text: "See what's new in v0.9.1"
                    quiet: true
                    onClicked: whatsNewDriver.presentPreUpdate()
                }

                // A second page control, so "does the focus chain belong to the
                // page again" is a question with an answer.
                ExoButton {
                    objectName: "pageOther"
                    text: "Check for updates"
                }
            }

            Loader {
                id: overlayLoader

                objectName: "overlayLoader"

                property Item focusReturn: null

                anchors.fill: parent
                active: shell.whatsNew.active
                onLoaded: {
                    const card = overlayLoader.item as WhatsNewOverlay;
                    overlayLoader.focusReturn = card !== null ? card.focusReturnItem : null;
                }
                onActiveChanged: {
                    if (overlayLoader.active)
                        return;
                    const target = overlayLoader.focusReturn;
                    overlayLoader.focusReturn = null;
                    if (target !== null && target.enabled && target.visible)
                        target.forceActiveFocus(Qt.OtherFocusReason);
                }

                sourceComponent: WhatsNewOverlay {
                    whatsNew: shell.whatsNew
                    contentTopInset: 40
                    focus: true
                }
            }
        }
    }

    function init() {
        whatsNewDriver.reset();
    }

    function makeShell() {
        let shell = createTemporaryObject(shellComponent, testCase, {
            width: 860,
            height: 700,
            whatsNew: whatsNewDriver.adapter
        });
        verify(shell);
        waitForRendering(shell);
        return shell;
    }

    function find(root, name) {
        let stack = [root];
        while (stack.length > 0) {
            let item = stack.pop();
            if (item.objectName === name)
                return item;
            for (let i = 0; i < item.children.length; ++i)
                stack.push(item.children[i]);
        }
        return null;
    }

    // Every match, in child order — "newest first" is a statement about the
    // sequence the reader sees, so the traversal has to preserve it.
    function collectByName(item, name, out) {
        if (out === undefined)
            out = [];
        if (item.objectName === name)
            out.push(item);
        for (let i = 0; i < item.children.length; ++i)
            collectByName(item.children[i], name, out);
        return out;
    }

    function overlay(shell) {
        return find(shell, "quickWhatsNewOverlay");
    }

    // The link the cutover left inert. Pressing it has to put the document on
    // screen — the whole defect was that this reached nothing at all.
    function test_the_card_link_opens_the_overlay() {
        let shell = makeShell();
        compare(overlay(shell), null, "the overlay is up before anything asked for it");

        let link = find(shell, "cardLink");
        verify(link);
        mouseClick(link);
        waitForRendering(shell);

        let card = overlay(shell);
        verify(card, "the Settings card link did not open the release-notes overlay");
        // Pre-update: the full channel list, and no suppress tick.
        compare(card.whatsNew.postUpdateMode, false);
    }

    // Escape means "leave this for now" on every in-window surface, and this one
    // has nothing to commit — so it is the whole answer.
    function test_escape_closes_and_hands_the_keyboard_back() {
        let shell = makeShell();
        let link = find(shell, "cardLink");
        verify(link);
        mouseClick(link);
        waitForRendering(shell);
        verify(overlay(shell), "the link did not open the overlay");
        verify(!link.activeFocus, "the open overlay left the keyboard on the page behind it");

        keyClick(Qt.Key_Escape);
        waitForRendering(shell);

        compare(overlay(shell), null, "Escape did not close the overlay");
        verify(link.activeFocus, "closing the overlay stranded the keyboard: focus did not return to the link");
    }

    function test_the_primary_action_closes_and_hands_the_keyboard_back() {
        let shell = makeShell();
        let link = find(shell, "cardLink");
        verify(link);
        mouseClick(link);
        waitForRendering(shell);

        verify(overlay(shell), "the link did not open the overlay");
        let close = find(shell, "whatsNewCloseButton");
        verify(close);
        mouseClick(close);
        waitForRendering(shell);

        compare(overlay(shell), null, "the primary action did not close the overlay");
        verify(link.activeFocus, "closing the overlay stranded the keyboard: focus did not return to the link");
    }

    // The one difference between the two entry points that the SURFACE owns. The
    // tick gates the automatic post-update show; offering it on a list the user
    // opened themselves would suggest that link can be turned off, and it cannot.
    function test_only_the_post_update_mode_offers_the_suppress_tick() {
        let shell = makeShell();

        whatsNewDriver.presentPreUpdate();
        waitForRendering(shell);
        verify(overlay(shell), "the pre-update entry point raised no overlay");
        let preTick = find(shell, "whatsNewShowAfterUpdates");
        verify(preTick === null || !preTick.visible,
               "the pre-update list offered a 'show after updates' tick");
        // The action a list the user asked for wants.
        compare(find(shell, "whatsNewCloseButton").text, "Close");

        keyClick(Qt.Key_Escape);
        waitForRendering(shell);

        whatsNewDriver.presentPostUpdate();
        waitForRendering(shell);
        verify(overlay(shell), "the post-update entry point raised no overlay");
        let tick = find(shell, "whatsNewShowAfterUpdates");
        verify(tick, "the post-update overlay is missing the 'show after updates' tick");
        verify(tick.checked, "the tick must be checked by default: notes are shown unless the user opts out");
        // Acknowledges what just happened, rather than merely dismissing a list.
        compare(find(shell, "whatsNewCloseButton").text, "Got it");
    }

    // Unticking persists `whats_new_suppressed`. The tick reads in the user's
    // direction ("show them") and the stored key is its inverse, so the mapping is
    // the whole risk here.
    function test_unticking_reports_the_suppressed_value_to_persist() {
        let shell = makeShell();
        whatsNewDriver.presentPostUpdate();
        waitForRendering(shell);

        let tick = find(shell, "whatsNewShowAfterUpdates");
        verify(tick, "the post-update overlay is missing its suppress tick");
        compare(whatsNewDriver.suppressEdits(), 0, "nothing was persisted before the user touched the tick");

        mouseClick(tick);
        waitForRendering(shell);

        compare(tick.checked, false);
        compare(whatsNewDriver.suppressEdits(), 1, "unticking persisted nothing");
        compare(whatsNewDriver.lastSuppressed(), true,
                "unticking 'show release notes after updates' must persist suppressed=true");

        mouseClick(tick);
        waitForRendering(shell);

        compare(whatsNewDriver.suppressEdits(), 2);
        compare(whatsNewDriver.lastSuppressed(), false, "re-ticking must un-suppress future auto-shows");
    }

    // Both entry points can legitimately have nothing: a channel with no releases
    // yet, a payload that carried no notes. An overlay whose entire content is
    // missing is worse than none.
    function test_no_notes_raises_nothing() {
        let shell = makeShell();

        whatsNewDriver.presentNothing();
        waitForRendering(shell);

        compare(overlay(shell), null, "an overlay was raised with no release notes to show");
    }

    // ONE always-expanded document: every release is on screen at once, newest
    // first, with no disclosure to open — and the bodies are Markdown, so a
    // `### Fixed` reaches the reader as a heading rather than as three hashes.
    function test_notes_are_one_expanded_document_newest_first_rendered_as_markdown() {
        let shell = makeShell();
        whatsNewDriver.presentPostUpdate();
        waitForRendering(shell);

        let card = overlay(shell);
        verify(card, "the post-update entry point raised no overlay");

        let versions = collectByName(card, "whatsNewNoteVersion");
        compare(versions.length, 2, "both releases must be in the document at once, with nothing to expand");
        compare(versions[0].text, "v0.9.1", "the newest release is not first");
        compare(versions[1].text, "v0.9.0");

        let bodies = collectByName(card, "whatsNewNoteBody");
        compare(bodies.length, 2);
        for (const body of bodies) {
            // The whole difference between a changelog and a paste of its source.
            compare(body.textFormat, Text.MarkdownText, "the release body is not rendered as Markdown");
            verify(body.visible, "a release body is not on screen; the document is not expanded");
        }
        verify(bodies[0].text.indexOf("The newest release.") !== -1);
    }

    // The other half of the modal focus contract: while the card is up, Tab may not
    // leave it (ExoOverlayCard's ring, asserted there) — and once it is closed, Tab
    // must belong to the page again. A surface that keeps folding the chain back
    // into itself after it is gone is a trap with nothing on screen to explain it.
    function test_the_page_owns_the_focus_chain_again_once_the_overlay_is_closed() {
        let shell = makeShell();
        let link = find(shell, "cardLink");
        let other = find(shell, "pageOther");
        verify(link);
        verify(other);

        mouseClick(link);
        waitForRendering(shell);
        verify(overlay(shell), "the link did not open the overlay");
        // Inside the card: Tab cannot reach the page control behind the scrim.
        for (let i = 0; i < 6; ++i) {
            keyClick(Qt.Key_Tab);
            verify(shell.Window.activeFocusItem !== other,
                   "Tab #" + (i + 1) + " left the modal overlay for a page control");
        }

        keyClick(Qt.Key_Escape);
        waitForRendering(shell);
        compare(overlay(shell), null, "Escape did not close the overlay");
        verify(link.activeFocus, "closing the overlay stranded the keyboard");

        keyClick(Qt.Key_Tab);

        compare(shell.Window.activeFocusItem, other,
                "Tab did not reach the next page control after the overlay closed");
    }
}
