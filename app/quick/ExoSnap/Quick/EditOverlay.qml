import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Edit/Output/Save surface (ADR 0022): an overlay over the Record page, not a
// navigation destination.
//
// One view — player, trim timeline, and a right rail carrying the details card
// plus the export panel. The post-flight report rides as a header badge. The
// rail is never hidden: it carries the export controls, so at the 860×700
// minimum window it narrows to 240 px and gives the width back to the player
// instead of disappearing.
Item {
    id: root

    required property EditSessionAdapter session
    required property EditTimelineAdapter timeline
    required property EditPlayerAdapter player
    required property EditExportAdapter exporter

    // Measured against the page, which is the client area minus the overlay's
    // 20 px band on each side — so an 860 px window reaches the page as 820 px
    // and lands on the narrow rail.
    readonly property int railWidth: ExoTheme.isWide(page.width) ? 320
                                   : ExoTheme.isRegular(page.width) ? 280 : 240

    objectName: "quickEditOverlay"

    // Blocks the surfaces underneath: a click that reaches the Record page while
    // the editor is up would act on a recording the user is no longer looking at.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.alpha(ExoTheme.background, 0.72)
    }

    Rectangle {
        id: page

        anchors {
            fill: parent
            margins: 20
        }
        color: ExoTheme.background
        border.width: 1
        border.color: ExoTheme.line
        radius: ExoTheme.radiusLg
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ---- Mode bar ----
            Rectangle {
                color: ExoTheme.surface
                Layout.fillWidth: true
                Layout.preferredHeight: 52

                RowLayout {
                    spacing: ExoTheme.spacingSm
                    anchors {
                        fill: parent
                        leftMargin: ExoTheme.spacingMd
                        rightMargin: ExoTheme.spacingMd
                    }

                    ExoButton {
                        objectName: "editOverlayBackButton"
                        text: qsTr("Back")
                        quiet: true
                        onClicked: root.requestClose()
                    }

                    Label {
                        text: qsTr("Edit & export")
                        textFormat: Text.PlainText
                        color: ExoTheme.text
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: 16
                            weight: Font.Bold
                        }
                    }

                    Label {
                        text: root.session.clipTitle
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        color: ExoTheme.accent
                        Layout.fillWidth: true
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: 12
                        }
                    }

                    ExoBadge {
                        id: reportBadge

                        text: root.session.reportLabel !== "" ? root.session.reportLabel : qsTr("Report")
                        tone: root.session.reportSeverity === EditSessionAdapter.Critical ? "blocker" : root.session.reportSeverity === EditSessionAdapter.Warning ? "notice" : "neutral"

                        HoverHandler {
                            id: reportHover
                        }

                        ToolTip.visible: reportHover.hovered
                        ToolTip.text: root.session.reportTooltip
                    }
                }

                Rectangle {
                    height: 1
                    color: ExoTheme.line
                    anchors {
                        left: parent.left
                        right: parent.right
                        bottom: parent.bottom
                    }
                }
            }

            // ---- Content: player + timeline, and the rail ----
            RowLayout {
                spacing: 0
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    spacing: ExoTheme.spacingMd
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.leftMargin: ExoTheme.spacingMd
                    Layout.rightMargin: ExoTheme.spacingSm
                    Layout.topMargin: ExoTheme.spacingMd
                    Layout.bottomMargin: ExoTheme.spacingMd

                    // The frame takes the height left over above the timeline and
                    // letterboxes inside it. Deriving it from the width instead
                    // left an empty band under a narrow window, because a narrow
                    // player is also a short one and nothing claimed the rest.
                    EditPlayer {
                        player: root.player
                        session: root.session
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 180
                    }

                    EditTimeline {
                        id: timelineView

                        session: root.session
                        timeline: root.timeline
                        player: root.player
                        Layout.fillWidth: true
                        Layout.preferredHeight: timelineView.implicitHeight
                    }
                }

                // The rail scrolls because both cards together outgrow the column
                // at the 700 px minimum height once a result shows — a clipped
                // "Show in Explorer" would be unreachable, a scrolled one is not.
                ExoScrollView {
                    id: rail

                    contentWidth: availableWidth
                    clip: true
                    // The vertical scroll bar is an overlay, so `availableWidth`
                    // still spans underneath it. Reserving the gutter -- the same
                    // token the Settings page reserves -- keeps the cards clear of
                    // it once a result makes the rail scroll. Unconditional: a
                    // conditional binding would feed back into the wrap height of
                    // the cards below. The width added here is the gutter, so the
                    // cards themselves still measure exactly `railWidth`.
                    rightPadding: ExoTheme.spacingLg
                    Layout.preferredWidth: root.railWidth + ExoTheme.spacingLg
                    Layout.fillHeight: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        spacing: ExoTheme.spacingMd
                        width: rail.availableWidth

                        EditDetailsRail {
                            session: root.session
                            // Only the narrowest breakpoint tightens the card;
                            // that is where the seven facts left the export panel
                            // below them without usable height.
                            compact: root.railWidth === 240
                            Layout.fillWidth: true
                            Layout.topMargin: ExoTheme.spacingMd
                        }

                        EditExportPanel {
                            exporter: root.exporter
                            Layout.fillWidth: true
                        }

                        Item {
                            Layout.fillHeight: true
                            Layout.minimumHeight: ExoTheme.spacingMd
                        }
                    }
                }
            }

            // ---- Action bar: the surface's single action, bottom-right ----
            Rectangle {
                color: ExoTheme.surface
                Layout.fillWidth: true
                Layout.preferredHeight: 64

                Rectangle {
                    height: 1
                    color: ExoTheme.line
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                    }
                }

                RowLayout {
                    spacing: ExoTheme.spacingSm
                    anchors {
                        fill: parent
                        leftMargin: ExoTheme.spacingMd
                        rightMargin: ExoTheme.spacingMd
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    ExoButton {
                        objectName: "editOverlayExportButton"
                        text: qsTr("Export")
                        enabled: root.exporter.canExport
                        Layout.minimumWidth: 150
                        onClicked: root.startExport()
                    }
                }
            }
        }
    }

    // "Overwrite original" finishes with an atomic replace, so the question comes
    // before the run starts, not as a report afterwards. The non-destructive
    // choice is the default, so a stray Enter never replaces a recording.
    ExoConfirmDialog {
        id: overwriteDialog

        title: qsTr("Overwrite original recording")
        bodyText: root.exporter.overwritePrompt
        onAccepted: root.exporter.startExport()
    }

    ExoConfirmDialog {
        id: discardDialog

        title: qsTr("Discard edits")
        bodyText: qsTr("The trim points and markers on this clip are not exported yet.")
        onAccepted: root.session.close()
    }

    function startExport(): void {
        if (root.exporter.overwriteSelected) {
            overwriteDialog.open();
            return;
        }
        root.exporter.startExport();
    }

    function requestClose(): void {
        // A running export is never silently abandoned, and neither is a trim the
        // user set but has not exported.
        if (root.exporter.running) {
            return;
        }
        if (root.session.hasUnsavedEdits) {
            discardDialog.open();
            return;
        }
        root.session.close();
    }

    Keys.onEscapePressed: root.requestClose()
}
