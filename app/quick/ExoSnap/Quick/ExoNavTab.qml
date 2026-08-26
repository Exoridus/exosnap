import QtQuick
import QtQuick.Controls.Basic

// A destination in the shell's title band.
//
// Underline selection rather than an outlined pill. Both were tried; side by
// side the pill draws a permanent rounded box around one word, which reads as a
// button that happens to be stuck down rather than as the page you are on, and
// it competes with the window buttons at the other end of the same 40 px band.
// The underline states the same thing with one mint rule and no enclosing shape.
//
// The pill's one real advantage was that it could carry the keyboard focus ring.
// That is kept — but only on visualFocus, so it appears when a keyboard user
// needs it instead of permanently.
Button {
    id: root

    // Sized to its label, not to a grid: the tabs, a wordmark, a status pill, a
    // bell and three window buttons all share one 40 px band at the 860 px
    // minimum window, and a per-tab minimum width is what pushed the close
    // button off the right edge there.
    // Measured at BOTH weights, and sized to the wider of the two, in every
    // state. Sizing the tab from the live label made every tab in the row shift
    // by the few pixels the weight change costs each time the selection moved;
    // sizing it from the selected weight alone assumed DemiBold is always the
    // wider one, which it is not — "Diagnostics" measures 73 px at Medium and
    // 72 at DemiBold, and the tab that was one pixel too narrow for its own
    // unselected label elided at every window width, 800 px of free space
    // included.
    implicitWidth: Math.ceil(Math.max(selectedMetrics.implicitWidth, unselectedMetrics.implicitWidth))
                   + leftPadding + rightPadding
    // The full 40 px band, not a shorter centred cell: the previous height left
    // a spacingXs sliver above and below the tab where the hover/press block and
    // the focus ring stopped short of the band's own top and bottom edges, while
    // the window buttons at the other end of the same band went edge to edge.
    implicitHeight: 40
    // All five destinations are direct tabs, so the band's width budget is the
    // one thing deciding this padding. Below the regular width class it drops a
    // rung — 8 px a side still leaves the shortest label ("Logs") a hit target
    // wider than it is tall, and it is the only lever that costs nothing:
    // shrinking the type or eliding the words would both make a destination
    // harder to read to buy the same pixels.
    leftPadding: root.compact ? ExoTheme.spacingSm : ExoTheme.spacingLg
    rightPadding: root.compact ? ExoTheme.spacingSm : ExoTheme.spacingLg
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true
    checkable: true
    autoExclusive: true
    focusPolicy: Qt.StrongFocus

    property alias selected: root.checked
    property bool compact: false

    Accessible.name: root.text

    contentItem: Label {
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: root.text
        textFormat: Text.PlainText
        // The LAST resort, and only that. The band gives up its drag handle
        // first, then the tabs' own padding at the compact rung, and only a
        // label that still does not fit after both elides — which happens with
        // a translation long enough that five destinations plus the window
        // buttons no longer share 860 px. The alternative measured worse: the
        // row laid itself out past the window's right edge and took the close
        // button with it. A destination the user can still recognise beats a
        // window they cannot close.
        elide: Text.ElideRight
        color: !root.enabled ? ExoTheme.textDim
               : root.selected || root.hovered ? ExoTheme.text : ExoTheme.textSecondary
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
            // The selected tab is the only bold one, so the weight change is the
            // second selection cue after the rule below.
            weight: root.selected ? Font.DemiBold : Font.Medium
        }
    }

    // Labels rather than TextMetrics, and that is not interchangeable either:
    // TextMetrics reports the sum of the glyph advances, while a Text item lays
    // the same string out and rounds the result up. Measuring with the item type
    // that actually draws is what makes the number the label's own.
    Label {
        id: selectedMetrics

        visible: false
        text: root.text
        textFormat: Text.PlainText
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
            weight: Font.DemiBold
        }
    }

    Label {
        id: unselectedMetrics

        visible: false
        text: root.text
        textFormat: Text.PlainText
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
            weight: Font.Medium
        }
    }

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }

    background: Item {
        // No corner radius: the tab fills its whole cell in the title band, the
        // same full-block treatment as the window buttons at the other end of
        // it. A rounded highlight here read as a floating chip rather than as
        // part of the same band.
        //
        // The already-selected tab is excluded from BOTH states, not just
        // hover: it already carries its own selection cues (the underline, the
        // bold label), so pressing it must not flash the hover/press block on
        // top of them.
        Rectangle {
            anchors.fill: parent
            color: root.down ? ExoTheme.surfaceHover : ExoTheme.surfaceRaised
            visible: (root.hovered || root.down) && !root.selected
        }

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.width: ExoTheme.focusRingWidth
            border.color: ExoTheme.text
            visible: root.visualFocus
        }

        Rectangle {
            height: 2
            color: ExoTheme.accent
            radius: 1
            visible: root.selected
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                leftMargin: ExoTheme.spacingXs
                rightMargin: ExoTheme.spacingXs
            }
        }
    }
}
