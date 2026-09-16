import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// A popup outlives the page it belongs to.
//
// Every popup in this application is declared inside the destination that owns
// it, but a Popup does not live in its parent's scene subtree -- it is reparented
// into the window's overlay. The shell swaps destinations by switching the
// visibility of a StackLayout child, which hides the anchor and leaves the popup
// drawing over the destination the user just navigated to. Nothing in Popup
// reacts to that on its own.
//
// `page` below is that StackLayout child: one item whose visibility stands for
// "this destination is on screen".
Item {
    id: root

    width: 420
    height: 320

    Item {
        id: page

        anchors.fill: parent

        ExoMenu {
            id: menu

            ExoMenuItem {
                text: "3 seconds"
            }
        }

        ExoSelect {
            id: select

            width: 200
            // `selectable` is what the adapter's option rows carry; a delegate
            // without it binds `enabled` to undefined.
            options: [{ label: "HEVC", value: "hevc", selectable: true },
                      { label: "AV1", value: "av1", selectable: true }]
            value: "hevc"
        }

        ExoInfoButton {
            id: info

            y: 120
            subject: "Bit depth"
            body: "10-bit is required for an HDR10 bitstream."
        }
    }

    TestCase {
        name: "PopupAnchorVisibilityTests"
        when: windowShown

        function cleanup() {
            menu.close();
            select.popup.close();
            page.visible = true;
        }

        function test_menuClosesWhenItsPageIsSwappedAway() {
            menu.open();
            verify(menu.opened, "the menu opens");

            page.visible = false;
            verify(!menu.opened, "a menu must not outlive the destination that owns it");
        }

        function test_selectPopupClosesWhenItsPageIsSwappedAway() {
            select.popup.open();
            verify(select.popup.opened, "the dropdown opens");

            page.visible = false;
            verify(!select.popup.opened, "a dropdown must not outlive the destination that owns it");
        }

        function test_infoPopoverClosesWhenItsPageIsSwappedAway() {
            info.clicked();
            verify(info.explaining, "the popover opens");

            page.visible = false;
            verify(!info.explaining, "a popover must not outlive the destination that owns it");
        }

        // The other half of the contract: coming back to a destination must not
        // re-open what the swap closed. A popup is opened by an interaction, and
        // restoring one the user never asked for again is its own defect.
        function test_returningToThePageLeavesThePopupsClosed() {
            menu.open();
            select.popup.open();
            page.visible = false;

            page.visible = true;
            verify(!menu.opened, "the menu stays closed on return");
            verify(!select.popup.opened, "the dropdown stays closed on return");
        }
    }
}
