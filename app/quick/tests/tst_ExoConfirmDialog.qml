pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The shared two-button confirm, asserted on the one thing a screenshot cannot
// show: whether the modal owns the keyboard while it is up.
//
// A Popup takes active focus only when it asks for it. Without that, everything
// this component documents about the keyboard is silently untrue — Escape is
// delivered to the window instead of resolving the guard, the button that
// `defaultIsCancel` marks never holds focus, and Tab and Return keep operating
// the page behind the scrim. All three look correct in a photograph.
TestCase {
    id: testCase

    name: "ExoConfirmDialog"
    when: windowShown
    width: 860
    height: 700
    visible: true

    Component {
        id: pageComponent

        Item {
            id: page

            property alias dialog: dialog
            property alias behind: behind

            ExoButton {
                id: behind
                objectName: "behind"
                text: "A control on the page"
            }

            ExoConfirmDialog {
                id: dialog

                title: "Discard the recording?"
                bodyText: "The clip that is still being written will be deleted."
                proceedText: "Discard"
                cancelText: "Keep recording"
            }
        }
    }

    SignalSpy {
        id: rejectedSpy
        signalName: "rejected"
    }

    function makePage() {
        let page = createTemporaryObject(pageComponent, testCase, { width: 860, height: 700 });
        verify(page);
        waitForRendering(page);
        return page;
    }

    function test_the_open_dialog_holds_the_keyboard() {
        let page = makePage();
        page.behind.forceActiveFocus();
        verify(page.behind.activeFocus, "the page control did not take focus to begin with");

        page.dialog.open();
        waitForRendering(page);

        verify(page.dialog.opened, "the dialog did not open");
        verify(page.dialog.activeFocus, "the modal dialog left the keyboard on the page behind it");
        verify(!page.behind.activeFocus, "the page control kept focus underneath the modal");
    }

    // Escape resolves to reject(), which is what makes "dismissing means keep the
    // window open" true. CloseOnEscape is documented to need activeFocus, so this
    // is the same contract as the test above seen from the outside.
    function test_escape_rejects_and_closes() {
        let page = makePage();
        page.dialog.open();
        waitForRendering(page);
        verify(page.dialog.opened);

        rejectedSpy.target = page.dialog;
        rejectedSpy.clear();

        keyClick(Qt.Key_Escape);
        waitForRendering(page);

        compare(rejectedSpy.count, 1, "Escape did not resolve the dialog as a rejection");
        verify(!page.dialog.opened, "Escape did not close the dialog");
    }

    // Tab must not walk out of the modal into the page it is covering. A popup that
    // holds focus is its own focus scope, so this follows from the fix rather than
    // from a sentinel — which is exactly why it is worth pinning.
    function test_tab_cannot_leave_the_modal_for_the_page() {
        let page = makePage();
        page.dialog.open();
        waitForRendering(page);
        verify(page.dialog.opened);

        for (let i = 0; i < 6; ++i) {
            keyClick(Qt.Key_Tab);
            verify(page.Window.activeFocusItem !== page.behind,
                   "Tab #" + (i + 1) + " left the modal dialog for a page control");
        }
    }
}
