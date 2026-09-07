import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The two behaviour changes a screenshot review cannot pin down on its own:
// the body's line clamp, and the dismiss glyph's rest-state visibility.
// Geometry and colour are judged with --visual-test; this file pins the
// properties that decide them.
//
// The window is never shown here: `visible` is gated on
// CaptureExclusion.granted, which stays false without a real platform window
// (see tst_OverlayQuickControlPill.qml) -- every assertion below is about the
// delegate's own properties, not about anything on screen.
TestCase {
    id: testCase

    name: "OverlayNotificationToast"
    when: windowShown
    width: 400
    height: 400
    visible: true

    Component {
        id: toastComponent

        OverlayNotificationToast {
            ListModel {
                id: toastModel

                ListElement {
                    sequence: 1
                    title: "Storage running low"
                    body: "Recording stopped."
                    tone: "error"
                    standing: true
                    remainingFraction: 1.0
                    actionCount: 1
                    primaryLabel: "Change folder"
                    primaryAction: 0
                    secondaryLabel: ""
                    secondaryAction: 0
                }
            }

            toasts: toastModel
        }
    }

    // No built-in findChild reaches into a Window's content tree from QML, so
    // the same recursive-by-objectName search tst_ExoOverlayCard.qml already
    // uses is repeated here.
    function find(root, objectName) {
        let stack = [root];
        while (stack.length > 0) {
            let item = stack.pop();
            if (item.objectName === objectName)
                return item;
            for (let i = 0; i < item.children.length; ++i)
                stack.push(item.children[i]);
        }
        return null;
    }

    function test_the_body_clamps_at_three_lines_not_six() {
        let toast = createTemporaryObject(toastComponent, testCase);
        verify(toast);

        let body = null;
        tryVerify(function () {
            body = find(toast.contentItem, "toastBody");
            return body !== null;
        });

        compare(body.maximumLineCount, 3);
    }

    // Dismissal is chrome, not an action: a quiet card at rest must show
    // nothing in its top-right corner. This is the state the shipped card got
    // wrong before this pass -- the glyph was always drawn.
    function test_the_dismiss_glyph_is_absent_at_rest() {
        let toast = createTemporaryObject(toastComponent, testCase);
        verify(toast);

        let dismiss = null;
        tryVerify(function () {
            dismiss = find(toast.contentItem, "toastDismiss");
            return dismiss !== null;
        });

        compare(dismiss.visible, false, "the dismiss glyph must stay empty until the card is hovered");
    }
}
