import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The row-explanation contract, tested as behaviour rather than as pixels.
//
// A settings row states its trade-off either in one visible line or behind this
// trigger, never both, and the trigger has to be a real control: reachable from
// the keyboard, dismissible with Escape, and announced with a name that says what
// it explains. A hover-only tooltip satisfies none of that, which is the whole
// reason this component exists rather than a ToolTip attached property.
Item {
    id: root

    width: 400
    height: 200

    Component {
        id: infoComponent

        ExoInfoButton {
            subject: "Constant quality (CQ)"
            body: "The value handed to the encoder, 1 to 51, where lower is better."
        }
    }

    TestCase {
        name: "ExoInfoButtonTests"
        when: windowShown

        function test_theTriggerNamesWhatItExplains() {
            const info = createTemporaryObject(infoComponent, root);
            verify(!!info, "component instantiates");
            verify(info.Accessible.name.indexOf("Constant quality (CQ)") !== -1,
                   "the accessible name has to carry the subject, not just 'info'");
            compare(info.Accessible.role, Accessible.Button);
            // The body reaches assistive technology without opening the panel: a
            // screen-reader user must not have to trigger a popup to hear it.
            compare(info.Accessible.description, info.body);
        }

        function test_itIsFocusableRatherThanHoverOnly() {
            const info = createTemporaryObject(infoComponent, root);
            compare(info.focusPolicy, Qt.StrongFocus);
        }

        function test_clickOpensAndEscapeCloses() {
            const info = createTemporaryObject(infoComponent, root);
            verify(!info.explaining, "closed at rest");

            mouseClick(info);
            tryVerify(() => info.explaining, 2000, "clicking the trigger opens its panel");

            keyClick(Qt.Key_Escape);
            tryVerify(() => !info.explaining, 2000, "Escape closes the panel");
        }

        function test_asecondClickClosesItAgain() {
            const info = createTemporaryObject(infoComponent, root);

            mouseClick(info);
            tryVerify(() => info.explaining, 2000, "first click opens");

            mouseClick(info);
            tryVerify(() => !info.explaining, 2000, "a second click on the trigger closes it again");
        }
    }
}
