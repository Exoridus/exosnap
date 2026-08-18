import QtQuick
import QtQuick.Controls.Basic
import QtTest

import ExoSnap.Quick.TestControls

// QCR-510: the drag target used to be the 6 px visual thumb, because `padding`
// inset the contentItem inside the 12 px bar. The fix has to widen the target
// WITHOUT changing either the drawing or the control's implicit size — every
// scrolling surface in the frontend reserves a gutter from that size.
TestCase {
    id: testCase

    name: "ExoScrollBar"
    when: windowShown
    width: 200
    height: 300
    visible: true

    Component {
        id: barComponent

        ExoScrollBar {
            orientation: Qt.Vertical
            height: 200
            size: 0.25
            policy: ScrollBar.AlwaysOn
        }
    }

    Component {
        id: horizontalComponent

        ExoScrollBar {
            orientation: Qt.Horizontal
            width: 200
            size: 0.25
            policy: ScrollBar.AlwaysOn
        }
    }

    function test_the_bar_still_measures_12px() {
        // The reserved gutter is derived from this. A wider bar would push every
        // scrolling surface's content in by the difference.
        let bar = createTemporaryObject(barComponent, testCase);
        verify(bar);
        compare(bar.implicitWidth, 12);
    }

    function test_the_whole_bar_width_is_the_interaction_target() {
        let bar = createTemporaryObject(barComponent, testCase);
        verify(bar);
        compare(bar.contentItem.width, 12);
    }

    function test_the_drawn_thumb_is_still_6px() {
        let bar = createTemporaryObject(barComponent, testCase);
        verify(bar);
        let thumb = bar.contentItem.children[0];
        verify(thumb, "the contentItem draws one thumb");
        compare(thumb.width, 6);
        // Centred, so the visual position of the bar is unchanged.
        compare(thumb.x + thumb.width / 2, bar.contentItem.width / 2);
    }

    function test_the_horizontal_bar_mirrors_the_rule() {
        let bar = createTemporaryObject(horizontalComponent, testCase);
        verify(bar);
        compare(bar.implicitHeight, 12);
        compare(bar.contentItem.height, 12);
        let thumb = bar.contentItem.children[0];
        verify(thumb);
        compare(thumb.height, 6);
        compare(thumb.y + thumb.height / 2, bar.contentItem.height / 2);
    }

    function test_a_bar_with_nothing_to_scroll_still_draws_nothing() {
        // The gutter is reserved whether or not a bar is drawn, so the handle
        // hides rather than the bar collapsing — unchanged by this item, and the
        // one thing the contentItem rewrite could have broken silently.
        let bar = createTemporaryObject(barComponent, testCase, { size: 1.0, policy: ScrollBar.AsNeeded });
        verify(bar);
        compare(bar.contentItem.visible, false);
    }
}
