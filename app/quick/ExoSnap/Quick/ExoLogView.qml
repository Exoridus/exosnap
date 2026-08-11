pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Virtualised log surface: monospace lanes (time · level · category · message),
// zebra rows, row selection with Ctrl+A / Ctrl+C, and sticky auto-scroll.
//
// A ListView over the filter proxy, NOT a document: the history is bounded at 5000
// entries and only the visible rows are ever instantiated, so appending stays O(1)
// in view work regardless of how long the session has been running.
Rectangle {
    id: root

    required property var model
    property bool autoScroll: true

    // Inclusive selection range over VISIBLE rows; -1 when nothing is selected.
    property int selectionAnchor: -1
    property int selectionEnd: -1

    signal copyRequested(int first, int last)
    signal copyAllRequested()

    color: ExoTheme.background
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusMd

    function selectRow(index: int, extend: bool): void {
        if (extend && root.selectionAnchor >= 0) {
            root.selectionEnd = index;
        } else {
            root.selectionAnchor = index;
            root.selectionEnd = index;
        }
    }

    ListView {
        id: list

        anchors {
            fill: parent
            margins: 1
        }
        clip: true
        model: root.model
        reuseItems: true
        boundsBehavior: Flickable.StopAtBounds
        focus: true
        activeFocusOnTab: true

        Accessible.role: Accessible.List
        Accessible.name: qsTr("Application log")

        ScrollBar.vertical: ExoScrollBar {}

        // Sticky tail: a new entry only pulls the view down while the user has
        // auto-scroll on, so reading history is never yanked away mid-line.
        onCountChanged: {
            if (root.autoScroll) {
                list.positionViewAtEnd();
            }
        }

        Keys.onPressed: function (event) {
            if (event.modifiers & Qt.ControlModifier) {
                if (event.key === Qt.Key_A) {
                    root.selectionAnchor = 0;
                    root.selectionEnd = list.count - 1;
                    event.accepted = true;
                } else if (event.key === Qt.Key_C) {
                    if (root.selectionAnchor >= 0) {
                        root.copyRequested(root.selectionAnchor, root.selectionEnd);
                    } else {
                        root.copyAllRequested();
                    }
                    event.accepted = true;
                }
            }
        }

        delegate: Rectangle {
            id: row

            required property int index
            required property string timestampText
            required property string severityKey
            required property string severityLabel
            required property string category
            required property string message

            readonly property bool selected: root.selectionAnchor >= 0
                                             && row.index >= Math.min(root.selectionAnchor, root.selectionEnd)
                                             && row.index <= Math.max(root.selectionAnchor, root.selectionEnd)
            readonly property color severityColor: row.severityKey === "warning" ? ExoTheme.warning
                                                 : (row.severityKey === "error" || row.severityKey === "critical") ? ExoTheme.error
                                                 : row.severityKey === "debug" ? ExoTheme.textDim
                                                 : ExoTheme.textSecondary

            width: list.width
            height: messageLabel.implicitHeight + 2 * 3
            // Zebra tint is one surface step up from the viewer's own ground — the
            // same "next surface" layering convention used for cards.
            color: row.selected ? ExoTheme.surfaceHover : (row.index % 2 === 1 ? ExoTheme.surface : "transparent")

            Rectangle {
                anchors {
                    top: parent.top
                    left: parent.left
                    right: parent.right
                }
                height: 1
                color: ExoTheme.line
                visible: row.index > 0
            }

            // MouseArea rather than a TapHandler: the shift-extend gesture needs the
            // event's keyboard modifiers, which a pointer handler's event point does
            // not carry.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onClicked: function (mouse) {
                    list.forceActiveFocus();
                    root.selectRow(row.index, (mouse.modifiers & Qt.ShiftModifier) !== 0);
                }
            }

            Label {
                id: timeLabel

                anchors {
                    top: parent.top
                    topMargin: 3
                    left: parent.left
                    leftMargin: ExoTheme.spacingSm
                }
                text: row.timestampText
                textFormat: Text.PlainText
                color: ExoTheme.textDim
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 11
                }
            }

            Label {
                id: levelLabel

                anchors {
                    top: parent.top
                    topMargin: 4
                    left: timeLabel.right
                    leftMargin: ExoTheme.spacingSm
                }
                // Fixed lane width so the category column always starts on the same
                // character position, whatever the level label's own length.
                width: 62
                text: "[" + row.severityLabel + "]"
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: row.severityColor
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 10
                }
            }

            Label {
                id: categoryLabel

                anchors {
                    top: parent.top
                    topMargin: 3
                    left: levelLabel.right
                    leftMargin: ExoTheme.spacingXs
                }
                // Fixed lane, like the level column: the message must always start on
                // the same x, whatever the category's own length.
                width: 140
                text: row.category === "" ? "" : "[" + row.category + "]"
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.textSecondary
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 11
                }
            }

            Label {
                id: messageLabel

                anchors {
                    top: parent.top
                    topMargin: 3
                    left: categoryLabel.right
                    leftMargin: ExoTheme.spacingSm
                    right: parent.right
                    rightMargin: ExoTheme.spacingMd
                }
                text: row.message
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: ExoTheme.text
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 11
                }
            }
        }
    }

    Label {
        anchors.centerIn: parent
        text: qsTr("No log entries match the current filter.")
        textFormat: Text.PlainText
        visible: list.count === 0
        color: ExoTheme.textMuted
        font {
            family: ExoTheme.sansFamily
            pixelSize: 12
        }
    }
}
