import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Edit/Output/Save surface (ADR 0022): still a LAYER over the Record page
// rather than a navigation destination — ownership of the clip, the decoder
// session and the export is unchanged — but presented as a WORKSPACE that
// occupies the normal content region rather than as a dialog floating above a
// dimmed application.
//
// What it used to be: a global scrim over the whole window, a 20 px band of
// dimmed application showing on every side, and inside that a rounded, bordered
// rectangle nearly the size of the window. Three cues that all say "modal
// dialog" about a surface that is not one: nothing about editing a finished
// recording is a question the application is waiting on an answer to, and the
// presentation covered the shell's own window buttons in the bargain. The shell
// (title band, navigation, status, bell, window buttons) stays visible above
// this item; see AppShell's editor Loader.
//
// One view — player, trim timeline, and a right rail carrying the details card
// plus the export panel. The post-flight report rides as a header status. The
// rail is never hidden: it carries the export controls, so at the 860×700
// minimum window it narrows to 240 px and gives the width back to the player
// instead of disappearing.
Item {
    id: root

    required property EditSessionAdapter session
    required property EditTimelineAdapter timeline
    required property EditPlayerAdapter player
    required property EditExportAdapter exporter

    // Measured against the workspace, which is now the full content width — an
    // 860 px window reaches it as 860 px and still lands on the narrow rail.
    readonly property int railWidth: ExoTheme.isWide(page.width) ? 320
                                   : ExoTheme.isRegular(page.width) ? 280 : 240

    objectName: "quickEditOverlay"

    // Blocks the surface underneath: a click that reached the Record page while
    // the editor is up would act on a recording the user is no longer looking
    // at. The page below is fully covered either way — this is about hover and
    // wheel events, which an opaque rectangle does not stop.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
    }

    Rectangle {
        id: page

        anchors.fill: parent
        // Meets the content bounds on the page background, with no frame of its
        // own: the structure below (header divider, panel borders, footer
        // divider) is what gives the workspace its shape.
        color: ExoTheme.background
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ---- Workspace header ----
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

                    // A navigation action, drawn as one. It was a bare "Back"
                    // text run, indistinguishable from the title beside it; the
                    // chevron is the shared one every disclosure and popover in
                    // the product already points with, rotated left. Drawn, not
                    // typeset — the wordmark font has no "‹".
                    ExoButton {
                        objectName: "editOverlayBackButton"
                        text: qsTr("Back")
                        quiet: true
                        compact: true
                        leftPadding: backChevron.width + 2 * ExoTheme.spacingSm
                        rightPadding: ExoTheme.spacingMd
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: root.requestClose()

                        ExoChevron {
                            id: backChevron

                            direction: 90
                            tone: ExoTheme.textSecondary
                            width: 12
                            height: 12
                            anchors {
                                left: parent.left
                                leftMargin: ExoTheme.spacingSm
                                verticalCenter: parent.verticalCenter
                            }
                        }
                    }

                    Label {
                        text: qsTr("Edit & export")
                        textFormat: Text.PlainText
                        color: ExoTheme.text
                        Layout.alignment: Qt.AlignVCenter
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontSectionTitle
                            weight: Font.Bold
                        }
                    }

                    // The one element on the row allowed to give up room, and it
                    // may shrink below its own implicit width — otherwise a deep
                    // recording folder would push Back off one end of the band
                    // and the report status off the other.
                    Label {
                        id: clipTitleLabel

                        text: root.session.clipTitle
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        color: ExoTheme.accent
                        // QCR-509. Middle-elided by design, so the untruncated
                        // name existed only under the pointer. The accessible
                        // name carries it whether or not it is truncated —
                        // which is also the case where a screen reader would
                        // otherwise read the ellipsis.
                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("Editing %1").arg(root.session.clipTitle)
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.alignment: Qt.AlignVCenter
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: ExoTheme.fontSecondary
                        }

                        HoverHandler {
                            id: clipTitleHover
                        }

                        ToolTip.visible: clipTitleHover.hovered && clipTitleLabel.truncated
                        ToolTip.text: root.session.clipTitle
                    }

                    // A STATUS, and only a status. This used to be one badge
                    // whose text became "Report" when nothing was wrong and
                    // "Warning"/"Critical" when something was — an element that
                    // reads as an action in one state and as a verdict in the
                    // other, with a tooltip as its only behaviour either way.
                    // The label names what is being reported; the badge carries
                    // the verdict and its severity. Neither is clickable,
                    // because there is nothing to click.
                    Label {
                        text: qsTr("Report")
                        textFormat: Text.PlainText
                        color: ExoTheme.textMuted
                        Layout.alignment: Qt.AlignVCenter
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontCaption
                        }
                    }

                    ExoBadge {
                        text: root.session.reportLabel
                        tone: root.session.reportSeverity === EditSessionAdapter.Critical ? "blocker"
                              : root.session.reportSeverity === EditSessionAdapter.Warning ? "notice" : "neutral"
                        Layout.alignment: Qt.AlignVCenter

                        Accessible.name: qsTr("Recording report: %1").arg(root.session.reportLabel)
                        Accessible.description: root.session.reportTooltip

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

                    // One workspace-level boundary around the timeline, matching
                    // the player above it and the rail beside it. The strip's
                    // own track has a hairline, but the label zone, the loading
                    // hint and the clock row sat outside it on the bare page
                    // background — so next to two bordered panels the timeline
                    // read as loose furniture rather than as the third area of
                    // the workspace. Deliberately ONE boundary: the individual
                    // tracks inside it stay unboxed.
                    Rectangle {
                        color: ExoTheme.surface
                        border.width: 1
                        border.color: ExoTheme.line
                        radius: ExoTheme.radiusLg
                        Layout.fillWidth: true
                        Layout.preferredHeight: timelineView.implicitHeight + 2 * ExoTheme.spacingSm

                        EditTimeline {
                            id: timelineView

                            session: root.session
                            timeline: root.timeline
                            player: root.player
                            anchors {
                                fill: parent
                                topMargin: ExoTheme.spacingSm
                                rightMargin: ExoTheme.spacingSm
                                bottomMargin: ExoTheme.spacingSm
                                leftMargin: ExoTheme.spacingSm
                            }
                        }
                    }
                }

                // The rail scrolls because both cards together outgrow the column
                // at the 700 px minimum height once a result shows — a clipped
                // "Show in folder" would be unreachable, a scrolled one is not.
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
                            onChooseAnotherFolderRequested: retryFolderDialog.open()
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

                    // Exporting is what this workspace is FOR, so when it is
                    // available it takes the accent fill — it was an outlined
                    // secondary control indistinguishable from a dismiss button,
                    // in a footer where it is the only action. `tone` falls back
                    // to neutral while export is unavailable, which is also what
                    // ExoButton does with a disabled primary, so the two never
                    // disagree.
                    ExoButton {
                        objectName: "editOverlayExportButton"
                        text: qsTr("Export")
                        tone: root.exporter.canExport ? "primary" : "neutral"
                        // The shape the Record page gives its own primary
                        // control, for the same reason: this is the action the
                        // workspace exists to perform, and it is the only one in
                        // the footer.
                        pill: true
                        enabled: root.exporter.canExport
                        Layout.minimumWidth: 150
                        Layout.alignment: Qt.AlignVCenter
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

    FolderDialog {
        id: retryFolderDialog

        title: qsTr("Choose another export folder")
        onAccepted: root.exporter.retryInFolder(selectedFolder)
    }

    function startExport(): void {
        if (root.exporter.overwriteSelected) {
            overwriteDialog.open();
            return;
        }
        root.exporter.startExport();
    }

    function requestClose(): void {
        root.session.close();
    }

    Keys.onEscapePressed: root.requestClose()
}
