pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Exclusive segmented selector (Logs severity: All / Info / Issues).
Rectangle {
    id: root

    required property var options
    property int currentIndex: 0

    signal selected(int index)

    // QCR-503. The segments were AbstractButtons with no focus policy: the
    // whole control was mouse-only. It is a radio group, so it takes ONE tab
    // stop and the arrows move within it — five separate tab stops for five
    // severities would be the wrong shape and would bloat the page's tab order.
    // Home/End jump to the ends, which is what a radio group does everywhere.
    activeFocusOnTab: root.options.length > 0

    // Named by the caller — every one of them sits under a labelled setting row
    // or beside a labelled control, so a name invented here would be wrong more
    // often than right.
    Accessible.role: Accessible.Grouping

    function step(delta: int): void {
        const count = root.options.length;
        if (count <= 0)
            return;
        const next = (root.currentIndex + delta + count) % count;
        if (next !== root.currentIndex)
            root.selected(next);
    }

    Keys.onLeftPressed: root.step(-1)
    Keys.onUpPressed: root.step(-1)
    Keys.onRightPressed: root.step(1)
    Keys.onDownPressed: root.step(1)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home && root.currentIndex !== 0) {
            root.selected(0);
            event.accepted = true;
        } else if (event.key === Qt.Key_End && root.currentIndex !== root.options.length - 1) {
            root.selected(root.options.length - 1);
            event.accepted = true;
        }
    }

    implicitWidth: row.implicitWidth + 6
    implicitHeight: 30
    color: ExoTheme.surface
    border.width: 1
    border.color: root.activeFocus ? ExoTheme.text : ExoTheme.line
    radius: ExoTheme.radiusSm

    RowLayout {
        id: row

        spacing: 0
        anchors {
            fill: parent
            margins: 3
        }

        Repeater {
            model: root.options

            AbstractButton {
                id: segment

                required property int index
                required property string modelData

                implicitWidth: Math.max(56, segmentLabel.implicitWidth + 2 * ExoTheme.spacingMd)
                hoverEnabled: true
                Layout.fillHeight: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: segment.modelData
                Accessible.checked: root.currentIndex === segment.index
                onClicked: root.selected(segment.index)

                background: Rectangle {
                    color: root.currentIndex === segment.index ? ExoTheme.surfaceHover
                         : segment.hovered ? ExoTheme.surfaceRaised
                         : "transparent"
                    // While the group has focus the SELECTED chip is what the
                    // arrows will move, so that is where the focus treatment
                    // belongs — the group's own outer ring says which control
                    // has focus, this says where inside it.
                    border.width: root.currentIndex === segment.index ? 1 : 0
                    border.color: root.currentIndex === segment.index && root.activeFocus
                                  ? ExoTheme.text : ExoTheme.accent
                    radius: ExoTheme.radiusXs
                }

                contentItem: Label {
                    id: segmentLabel

                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: segment.modelData
                    textFormat: Text.PlainText
                    color: root.currentIndex === segment.index ? ExoTheme.text : ExoTheme.textSecondary
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontSecondary
                        weight: root.currentIndex === segment.index ? Font.DemiBold : Font.Medium
                    }
                }
            }
        }
    }
}
