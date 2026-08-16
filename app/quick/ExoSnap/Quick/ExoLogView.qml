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

    // Inclusive selection range over the entries' own SEQUENCE numbers, not
    // over row indices. A row index means nothing across a model change: the
    // history evicts from the front, the filter re-maps every visible row, and
    // a Clear empties the model — after any of those, index 4 is a different
    // entry than the one the user clicked, or no entry at all, and an
    // index-based selection silently follows along to whatever moved into that
    // slot. Sequences are assigned once per entry and never reused, and the
    // view is never sorted, so "between these two sequences" is the same set of
    // rows as "between these two indices" for as long as both still exist and
    // degrades correctly when they do not.
    //
    // -1 when nothing is selected.
    property real selectionAnchorSequence: -1
    property real selectionEndSequence: -1
    // Ctrl+A is "everything visible", which is not a range: it has to keep
    // meaning everything after a filter change or an append, and a range
    // captured from the rows that happened to be visible then would not.
    property bool selectionIsAll: false

    readonly property real selectionLowSequence: Math.min(root.selectionAnchorSequence, root.selectionEndSequence)
    readonly property real selectionHighSequence: Math.max(root.selectionAnchorSequence, root.selectionEndSequence)
    readonly property bool hasSelection: root.selectionIsAll || root.selectionAnchorSequence >= 0

    signal copyRequested(real firstSequence, real lastSequence)
    signal copyAllRequested()

    color: ExoTheme.background
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusMd

    function selectRow(sequence: real, extend: bool): void {
        if (extend && root.selectionAnchorSequence >= 0) {
            root.selectionEndSequence = sequence;
        } else {
            root.selectionAnchorSequence = sequence;
            root.selectionEndSequence = sequence;
        }
        root.selectionIsAll = false;
    }

    function clearSelection(): void {
        root.selectionAnchorSequence = -1;
        root.selectionEndSequence = -1;
        root.selectionIsAll = false;
    }

    // A reset replaces the history wholesale — Clear, or the harness seeding a
    // synthetic log — and no entry the selection named is in it. Eviction and
    // filtering are deliberately NOT here: those are row removals the
    // sequence-based selection already survives correctly, keeping whatever is
    // still shown.
    //
    // Bound to the model's own signal rather than to the view's row count: a
    // ListView recomputes `count` when it next lays out, so a model that fills
    // and empties again between two frames never reports the rows it briefly
    // had — and the emptied view would keep a selection nothing in it matches.
    Connections {
        target: root.model

        function onModelReset(): void {
            root.clearSelection();
        }
    }

    ListView {
        id: list

        // The item that actually holds the keyboard focus: Ctrl+A and Ctrl+C are
        // handled here, not on the frame around it. Named so a test can put the
        // focus where a click would.
        objectName: "logList"

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
            // The other way a view can end up empty: every row removed one at a
            // time, or filtered away. A range selection over sequences is
            // harmless there — it simply matches nothing — but "select all"
            // would go on meaning "all", and would take in whatever the session
            // logs next.
            if (list.count === 0)
                root.selectionIsAll = false;
            if (root.autoScroll) {
                list.positionViewAtEnd();
            }
        }

        Keys.onPressed: function (event) {
            if (event.modifiers & Qt.ControlModifier) {
                if (event.key === Qt.Key_A) {
                    root.selectionAnchorSequence = -1;
                    root.selectionEndSequence = -1;
                    root.selectionIsAll = true;
                    event.accepted = true;
                } else if (event.key === Qt.Key_C) {
                    if (root.selectionIsAll || root.selectionAnchorSequence < 0) {
                        root.copyAllRequested();
                    } else {
                        root.copyRequested(root.selectionLowSequence, root.selectionHighSequence);
                    }
                    event.accepted = true;
                }
            }
        }

        delegate: Rectangle {
            id: row

            required property int index
            required property var sequence
            required property string timestampText
            required property string severityKey
            required property string severityLabel
            required property string category
            required property string message

            readonly property bool selected: root.selectionIsAll
                                             || (root.selectionAnchorSequence >= 0
                                                 && row.sequence >= root.selectionLowSequence
                                                 && row.sequence <= root.selectionHighSequence)
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
                    root.selectRow(row.sequence, (mouse.modifiers & Qt.ShiftModifier) !== 0);
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
                    pixelSize: ExoTheme.fontCaption
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
                    pixelSize: ExoTheme.fontCaption
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
                    pixelSize: ExoTheme.fontCaption
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
            pixelSize: ExoTheme.fontSecondary
        }
    }
}
