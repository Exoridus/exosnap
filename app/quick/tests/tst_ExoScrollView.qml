import QtQuick
import QtQuick.Controls.Basic
import QtTest

import ExoSnap.Quick.TestControls

// A wheel notch has to move the surface by Windows' own scroll-lines setting
// (SPI_GETWHEELSCROLLLINES, ExoTheme.wheelScrollLines) times a body line,
// rather than by Qt Quick's own much smaller per-notch default.
TestCase {
    id: testCase

    name: "ExoScrollView"
    when: windowShown
    width: 200
    height: 200
    visible: true

    Component {
        id: viewComponent

        ExoScrollView {
            width: 200
            height: 200

            Column {
                width: 180

                Repeater {
                    model: 200

                    Rectangle {
                        width: 180
                        height: 20
                        color: "transparent"
                    }
                }
            }
        }
    }

    Component {
        id: horizontalViewComponent

        ExoScrollView {
            width: 200
            height: 200
            contentWidth: 4000
            contentHeight: 100

            Item {
                width: 4000
                height: 100
            }
        }
    }

    function test_one_notch_moves_wheelScrollLines_body_lines() {
        let view = createTemporaryObject(viewComponent, testCase);
        verify(view);
        compare(view.flickable.contentY, 0);

        const expectedStep = ExoTheme.wheelScrollLines * ExoTheme.bodyLineHeight;
        mouseWheel(view, view.width / 2, view.height / 2, 0, -120);
        // A downward notch (negative angleDelta.y) scrolls the content DOWN,
        // i.e. contentY increases -- the same direction Windows itself moves.
        fuzzyCompare(view.flickable.contentY, expectedStep, 0.5);
    }

    function test_notch_direction_matches_wheel_rotation() {
        let view = createTemporaryObject(viewComponent, testCase);
        verify(view);
        view.flickable.contentY = 500;

        mouseWheel(view, view.width / 2, view.height / 2, 0, 120);
        verify(view.flickable.contentY < 500, "rotating the wheel up scrolls the content up");
    }

    function test_scroll_never_passes_the_content_bounds() {
        let view = createTemporaryObject(viewComponent, testCase);
        verify(view);
        // Fewer notches than it would take to reach the end, at an exaggerated
        // delta so a clamping bug would visibly overshoot rather than land
        // just short of the boundary by coincidence.
        for (let i = 0; i < 50; ++i)
            mouseWheel(view, view.width / 2, view.height / 2, 0, -2400);
        const maximum = Math.max(0, view.flickable.contentHeight - view.flickable.height);
        compare(view.flickable.contentY, maximum);
    }

    function test_shift_wheel_scrolls_horizontally() {
        let view = createTemporaryObject(horizontalViewComponent, testCase);
        verify(view);
        compare(view.flickable.contentX, 0);

        const expectedStep = ExoTheme.wheelScrollLines * ExoTheme.bodyLineHeight;
        mouseWheel(view, view.width / 2, view.height / 2, 0, -120, Qt.NoButton, Qt.ShiftModifier);
        fuzzyCompare(view.flickable.contentX, expectedStep, 0.5);
        compare(view.flickable.contentY, 0, "a shift-modified notch must not also move the vertical position");
    }

    function test_scrollToEnd_and_scrollToHome_still_work() {
        let view = createTemporaryObject(viewComponent, testCase);
        verify(view);
        verify(view.scrollToEnd());
        verify(view.flickable.contentY > 0);
        verify(view.scrollToHome());
        compare(view.flickable.contentY, 0);
    }
}
