pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// The Record control: one accent pill split into a main face and a chevron face.
//
//     [ ○ Record | ⌄ ]
//
// The main face always starts NOW. The chevron offers the three countdown
// delays. That is the Widgets shell's behaviour, restored: the migration had
// replaced it with a separate `[Now ⌄]` combo box parked to the left of the
// button, which put a persistent selector on screen for a value that is 0 on
// almost every start and read as an unrelated control rather than as part of
// the action.
//
// Rebuilt in QML rather than ported: none of the Widgets styling carries over,
// only the interaction.
Item {
    id: root

    required property RecordViewModelAdapter recordViewModel
    // One rung down at the 860 px minimum window, in step with the rest of the
    // transport — see RecordActionButton.
    property bool compact: false
    readonly property bool counting: root.recordViewModel.countdownActive
    readonly property bool busy: root.recordViewModel.preparing || root.recordViewModel.finalizing
    readonly property bool mainEnabled: root.counting || root.recordViewModel.canStart
    // While a countdown runs the main face is the way OUT of it, so offering
    // "start in 5 seconds" beside it would be two answers to one question.
    readonly property bool chevronEnabled: !root.counting && !root.busy && root.recordViewModel.canStart

    // The countdown is the one state that keeps the accent FILL: cancelling it is
    // unambiguously the action in flight, whatever else is on the bar.
    //
    // It used to take the ERROR fill, which said "destructive" about a button
    // that stops something from starting. Nothing has been recorded yet, no file
    // exists and nothing is lost; the countdown's one available action is simply
    // its primary action, and it is drawn like one. The face's own glyph carries
    // the difference — a cross rather than the record dot — so Cancel and Record
    // are never confused at a glance despite sharing a colour.
    // Outlined, not filled -- the same treatment Stop wears while paused, which
    // is the loudest the bar ever needs a resting action to be. A solid accent
    // slab is the transport's way of saying "this is happening now": it belongs
    // to Stop and to Resume, and Record borrowing it made the idle bar shout
    // about a recording that has not started.
    //
    // A countdown is the exception and keeps the fill, because then something IS
    // happening and cancelling it is the one available action.
    readonly property bool outlined: !root.counting
    readonly property color fill: root.outlined ? ExoTheme.surfaceRaised : ExoTheme.accent
    readonly property color ink: root.outlined ? ExoTheme.accent : ExoTheme.accentInk

    // -1 is "no fraction measured yet", which covers all of Stopping and the
    // first instant of Saving. The bare "Finalizing…" is what that must read as;
    // a percentage only appears once the remuxer has actually counted something.
    readonly property string finalizingText: root.recordViewModel.savingProgress >= 0
                                             ? qsTr("Finalizing… %1%").arg(Math.round(root.recordViewModel.savingProgress * 100))
                                             : qsTr("Finalizing…")

    readonly property string mainText: root.counting ? qsTr("Cancel")
                                     : root.recordViewModel.preparing ? qsTr("Preparing…")
                                     : root.recordViewModel.finalizing ? root.finalizingText : qsTr("Record")

    objectName: "quickRecordSplitButton"
    implicitHeight: root.compact ? ExoTheme.controlHeight : ExoTheme.controlHeightLarge
    // The divider is a child of the Row below, and a positioner skips invisible
    // children — so counting it unconditionally declared the pill 1 px wider
    // than it composes in exactly the states where the divider is hidden.
    implicitWidth: mainFace.implicitWidth + (divider.visible ? divider.width : 0) + chevronFace.width

    Rectangle {
        id: pill

        anchors.fill: parent
        // Unavailable drops to the dock's own fill, the same step the round peers
        // take. It used to be `surfaceRaised` — which is now what an AVAILABLE
        // round control sits on, so a Record button that cannot start would have
        // read as the most ordinary control on the bar.
        color: root.mainEnabled || root.busy ? root.fill : ExoTheme.surface
        // An outlined pill sits on the dock's raised-control fill, the same step
        // the round peers take — without a border it would dissolve into them
        // instead of reading as a control of its own.
        border.width: (root.mainEnabled || root.busy) && !root.outlined ? 0 : 1
        // The accent hairline is what carries "primary" once the fill is gone. An
        // unavailable Record keeps the ordinary line instead: a control that
        // cannot start must not be the most emphasised thing on the bar.
        border.color: root.outlined && (root.mainEnabled || root.busy) ? ExoTheme.accent : ExoTheme.line
        radius: height / 2
    }

    Row {
        anchors.fill: parent

        AbstractButton {
            id: mainFace

            // Room for the widest state label, so the pill does not resize under
            // the pointer when the recording moves from Record to Preparing….
            implicitWidth: Math.max(root.compact ? 112 : 132,
                                    mainRow.implicitWidth + 2 * (root.compact ? ExoTheme.spacingLg
                                                                             : ExoTheme.spacingXl))
            height: parent.height
            hoverEnabled: true
            enabled: root.mainEnabled
            focusPolicy: Qt.StrongFocus
            Accessible.role: Accessible.Button
            Accessible.name: root.counting ? qsTr("Cancel countdown")
                             : root.recordViewModel.preparing ? qsTr("Preparing recording")
                             : root.recordViewModel.finalizing ? qsTr("Finalizing recording")
                                                               : qsTr("Start recording")
            onClicked: root.startNow()

            HoverHandler {
                cursorShape: root.mainEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            }

            background: Rectangle {
                color: mainFace.down ? Qt.darker(root.fill, 1.14)
                     : mainFace.hovered && mainFace.enabled ? Qt.lighter(root.fill, 1.08) : "transparent"
                // Only the outer end is round: the inner end meets the divider.
                topLeftRadius: height / 2
                bottomLeftRadius: height / 2
                visible: mainFace.hovered || mainFace.down
            }

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 2
                border.color: ExoTheme.text
                radius: height / 2
                visible: mainFace.visualFocus
            }

            Row {
                id: mainRow

                spacing: ExoTheme.spacingSm
                anchors.centerIn: parent

                // Only Cancel draws a glyph. The record dot beside the word
                // "Record" restated the label and read as a stray bullet at the
                // pill's size; the cross is what keeps Cancel from being read as
                // Record at a glance, which is the job the glyph was there for.
                ExoGlyph {
                    kind: ExoGlyph.Close
                    color: root.mainEnabled || root.busy ? root.ink : ExoTheme.textDim
                    width: 16
                    height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.counting && !root.busy
                }

                Label {
                    text: root.mainText
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.mainEnabled || root.busy ? root.ink : ExoTheme.textDim
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: root.compact ? ExoTheme.fontBody : ExoTheme.fontSectionTitle
                        weight: Font.DemiBold
                    }
                }
            }
        }

        Rectangle {
            id: divider

            width: 1
            height: parent.height - 2 * ExoTheme.spacingMd
            color: Qt.alpha(root.ink, 0.35)
            anchors.verticalCenter: parent.verticalCenter
            visible: root.chevronEnabled || root.mainEnabled
        }

        AbstractButton {
            id: chevronFace

            width: root.compact ? 34 : 40
            height: parent.height
            hoverEnabled: true
            enabled: root.chevronEnabled
            focusPolicy: Qt.StrongFocus
            Accessible.role: Accessible.ButtonMenu
            Accessible.name: qsTr("Start with a countdown")
            ToolTip.text: qsTr("Start with a countdown")
            ToolTip.visible: chevronFace.hovered && !countdownMenu.opened
            ToolTip.delay: 600
            onClicked: countdownMenu.opened ? countdownMenu.close() : root.openCountdownMenu()

            background: Rectangle {
                color: chevronFace.down ? Qt.darker(root.fill, 1.14)
                     : (chevronFace.hovered || countdownMenu.opened) && chevronFace.enabled ? Qt.lighter(root.fill, 1.08)
                                                                                            : "transparent"
                topRightRadius: height / 2
                bottomRightRadius: height / 2
                visible: chevronFace.hovered || chevronFace.down || countdownMenu.opened
            }

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 2
                border.color: ExoTheme.text
                radius: height / 2
                visible: chevronFace.visualFocus
            }

            ExoChevron {
                anchors.centerIn: parent
                tone: chevronFace.enabled ? root.ink : ExoTheme.textDim
                direction: countdownMenu.opened ? 180 : 0
                width: 14
                height: 14
            }

            HoverHandler {
                id: chevronHover

                cursorShape: root.chevronEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }
    }

    // Hover opens it, as in the Widgets shell. It deliberately does NOT close on
    // un-hover: the pointer has to leave the chevron to reach the menu, and a
    // menu that closes on the way to itself is unusable. Selection, Escape and a
    // press outside all close it.
    Timer {
        interval: 250
        running: chevronHover.hovered && chevronFace.enabled && !countdownMenu.opened
        onTriggered: root.openCountdownMenu()
    }

    ExoMenu {
        id: countdownMenu

        // Above the button, right-aligned to it: the transport sits at the bottom
        // edge of the window, so a menu dropping downwards would open off-screen.
        y: -height - ExoTheme.spacingXs
        x: root.width - width

        ExoMenuItem {
            text: qsTr("3 seconds")
            onTriggered: root.startAfter(3)
        }

        ExoMenuItem {
            text: qsTr("5 seconds")
            onTriggered: root.startAfter(5)
        }

        ExoMenuItem {
            text: qsTr("10 seconds")
            onTriggered: root.startAfter(10)
        }
    }

    function openCountdownMenu(): void {
        if (chevronFace.enabled)
            countdownMenu.open();
    }

    // Both paths go through the same pair of calls, so "start now" and "start in
    // 5 s" cannot drift apart. requestCountdownSeconds is synchronous here: it
    // persists the choice and re-synchronises the record state before
    // requestStart reads it.
    function startNow(): void {
        if (root.counting) {
            // requestStart() is the cancel too — the engine owns that transition.
            root.recordViewModel.requestStart();
            return;
        }
        root.recordViewModel.requestCountdownSeconds(0);
        root.recordViewModel.requestStart();
    }

    function startAfter(seconds: int): void {
        countdownMenu.close();
        root.recordViewModel.requestCountdownSeconds(seconds);
        root.recordViewModel.requestStart();
    }
}
