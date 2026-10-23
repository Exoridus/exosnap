pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Export as a context of the right panel. A view of EditExportAdapter only:
// options, review, job state and result all live there, so closing the panel or
// switching context never changes the job, and reopening shows it as it is.
ColumnLayout {
    id: root
    objectName: "editExportPanel"
    required property EditSessionAdapter session
    required property EditExportAdapter exporter
    readonly property int jobState: exporter.state
    readonly property bool settings: jobState === EditExportAdapter.Options || jobState === EditExportAdapter.Review
    readonly property bool working: jobState === EditExportAdapter.Preparing || jobState === EditExportAdapter.Running
        || jobState === EditExportAdapter.Cancelling
    readonly property var outcome: exporter.reviewOutcome
    readonly property bool planned: outcome.audioSummary !== undefined
    readonly property bool mixed: (outcome.outputAudioTracks || []).some(track => track.role === "mix")
    spacing: 0

    // Keyboard opening lands on what the state is about: the first setting, the
    // running job's cancel action, or the first result action.
    function focusFirst(): void {
        const target = root.settings ? formatSelect : primaryAction.visible && primaryAction.enabled ? primaryAction : secondaryAction;
        target.forceActiveFocus(Qt.TabFocusReason);
    }

    function stateText(): string {
        switch (root.jobState) {
        case EditExportAdapter.Preparing: return qsTr("Preparing…");
        case EditExportAdapter.Running: return qsTr("Exporting %1%").arg(root.exporter.progressPercent);
        case EditExportAdapter.Cancelling: return qsTr("Cancelling…");
        case EditExportAdapter.Done: return qsTr("Export complete");
        case EditExportAdapter.Failed: return qsTr("Export failed");
        case EditExportAdapter.Review: return qsTr("Ready to export");
        default: return "";
        }
    }

    component FieldLabel: Label {
        Layout.fillWidth: true
        color: ExoTheme.textSecondary
        font.family: ExoTheme.sansFamily
        font.pixelSize: ExoTheme.fontCaption
        wrapMode: Text.Wrap
    }
    component BodyText: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: ExoTheme.textSecondary
        font.family: ExoTheme.sansFamily
        font.pixelSize: ExoTheme.fontSecondary
    }
    component Divider: Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: ExoTheme.line
    }
    // One of two stacked result choices: a title, a sub line and, when it cannot
    // be chosen, the reason in place of the sub line.
    component ResultOption: Button {
        id: option
        property string detail: ""
        property string disabledReason: ""
        Layout.fillWidth: true
        checkable: true
        focusPolicy: Qt.TabFocus
        padding: ExoTheme.spacingSm
        Accessible.name: text
        Accessible.description: enabled ? detail : disabledReason
        contentItem: ColumnLayout {
            spacing: 2
            Label {
                Layout.fillWidth: true
                text: option.text
                wrapMode: Text.Wrap
                color: option.enabled ? ExoTheme.text : ExoTheme.textDim
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontBody
            }
            Label {
                Layout.fillWidth: true
                text: option.enabled ? option.detail : option.disabledReason
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: ExoTheme.textSecondary
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontCaption
            }
        }
        background: Rectangle {
            radius: ExoTheme.radiusSm
            color: option.checked ? ExoTheme.surfaceRaised : option.hovered ? ExoTheme.hoverTint(ExoTheme.surface) : "transparent"
            border.width: 1
            border.color: option.visualFocus ? ExoTheme.text : option.checked ? ExoTheme.accent : ExoTheme.line
        }
    }
    component Banner: Rectangle {
        id: banner
        property color tint: ExoTheme.accent
        property int glyph: ExoGlyph.Check
        property alias text: bannerText.text
        Layout.fillWidth: true
        implicitHeight: bannerRow.implicitHeight + 2 * ExoTheme.spacingSm
        radius: ExoTheme.radiusSm
        color: Qt.rgba(tint.r, tint.g, tint.b, 0.14)
        RowLayout {
            id: bannerRow
            anchors.fill: parent
            anchors.margins: ExoTheme.spacingSm
            spacing: ExoTheme.spacingSm
            ExoGlyph {
                Layout.alignment: Qt.AlignTop
                kind: banner.glyph
                color: banner.tint
                Accessible.ignored: true
            }
            Label {
                id: bannerText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: ExoTheme.text
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontSecondary
            }
        }
    }

    ExoScrollView {
        id: body
        Layout.fillWidth: true
        Layout.fillHeight: true
        contentWidth: availableWidth
        clip: true
        rightPadding: ExoTheme.spacingMd
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: body.availableWidth
            spacing: ExoTheme.spacingMd

            // Settings. Every field writes straight into the adapter's options.
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.settings
                spacing: ExoTheme.spacingSm
                FieldLabel { text: qsTr("Format") }
                ExoSelect {
                    id: formatSelect
                    objectName: "editExportFormat"
                    Layout.fillWidth: true
                    options: root.exporter.containerOptions.concat([{value: "webm", label: "WebM", selectable: false,
                        reason: qsTr("WebM export is not available yet.")}])
                    value: root.exporter.containerKey
                    Accessible.name: qsTr("Export format")
                    onValueActivated: value => root.exporter.containerKey = value
                }
                BodyText {
                    objectName: "editExportDestinationHint"
                    text: root.exporter.destinationText
                    font.pixelSize: ExoTheme.fontCaption
                }
                Divider {}
                FieldLabel { text: qsTr("Range") }
                ExoSelect {
                    objectName: "editExportSpan"
                    Layout.fillWidth: true
                    options: [
                        {value: "entire", label: qsTr("Entire timeline"), selectable: true},
                        {value: "occupied", label: qsTr("Occupied content"), selectable: true},
                        {value: "result", label: qsTr("Result range"), selectable: true},
                        {value: "time", label: qsTr("I/O time range"), selectable: true},
                        {value: "selected", label: qsTr("Selected clips"), selectable: true}
                    ]
                    value: root.exporter.spanKey
                    Accessible.name: qsTr("Export range")
                    onValueActivated: value => {
                        root.exporter.setTimeSelectionUs(root.session.timeSelectionInUs, root.session.timeSelectionOutUs);
                        root.exporter.spanKey = value;
                    }
                }
                FieldLabel { text: qsTr("Size") }
                ExoSelect {
                    objectName: "editExportProfiles"
                    Layout.fillWidth: true
                    options: root.exporter.profileOptions
                    value: root.exporter.profileKey
                    Accessible.name: qsTr("Export size")
                    onValueActivated: value => root.exporter.profileKey = value
                }
                Divider {}
                FieldLabel { text: qsTr("Result") }
                ResultOption {
                    objectName: "editExportFinishedStereo"
                    text: qsTr("Finished stereo mix")
                    detail: qsTr("Enabled audio tracks mixed into one stereo track, as you hear it.")
                    checked: root.exporter.modeKey === "finishedStereo"
                    onClicked: {
                        root.exporter.modeKey = "finishedStereo";
                        checked = Qt.binding(() => root.exporter.modeKey === "finishedStereo");
                    }
                }
                ResultOption {
                    objectName: "editExportOriginalStreams"
                    text: qsTr("Keep original streams")
                    detail: qsTr("Copies the source audio tracks unchanged where the edit allows it.")
                    checked: root.exporter.modeKey === "originalStreams"
                    onClicked: {
                        root.exporter.modeKey = "originalStreams";
                        checked = Qt.binding(() => root.exporter.modeKey === "originalStreams");
                    }
                }
            }

            // Summary of the prepared export, formerly the modal review.
            Rectangle {
                objectName: "editExportSummary"
                Layout.fillWidth: true
                visible: root.jobState === EditExportAdapter.Review
                implicitHeight: summary.implicitHeight + 2 * ExoTheme.spacingSm
                radius: ExoTheme.radiusSm
                color: ExoTheme.surfaceRaised
                ColumnLayout {
                    id: summary
                    anchors.fill: parent
                    anchors.margins: ExoTheme.spacingSm
                    spacing: ExoTheme.spacingXs
                    BodyText {
                        color: ExoTheme.text
                        text: qsTr("%1 to %2 · %3 video · %4")
                            .arg(root.outcome.path === "Copy" ? qsTr("Copy") : qsTr("Render"))
                            .arg(root.outcome.container || "")
                            .arg(root.outcome.videoCodec || "")
                            .arg(root.outcome.outputFile || "")
                    }
                    BodyText {
                        objectName: "editExportAudioSummary"
                        visible: root.planned
                        text: qsTr("Audio: %1").arg(root.outcome.audioSummary || "")
                    }
                    BodyText {
                        visible: root.outcome.path === "Copy" && !root.planned
                        text: qsTr("All %1 source audio tracks will be copied: %2")
                            .arg((root.outcome.outputAudioTracks || []).length)
                            .arg((root.outcome.outputAudioTracks || []).map(track => track.name).join(", ") || qsTr("none"))
                    }
                    BodyText {
                        visible: root.planned ? root.mixed : root.outcome.path === "Render"
                        text: root.outcome.videoOnly
                            ? qsTr("Video-only render. No audio tracks will be included.")
                            : root.planned
                                ? qsTr("Enabled audio (%1) is mixed into one 48 kHz stereo %3 track: %2")
                                    .arg((root.outcome.selectedTracks || []).length)
                                    .arg((root.outcome.selectedTracks || []).map(track => track.name).join(", "))
                                    .arg(((root.outcome.outputAudioTracks || [])[0] || {}).codec || "")
                                : qsTr("Selected tracks (%1) will be mixed into one 48 kHz stereo PCM24 track: %2")
                                    .arg((root.outcome.selectedTracks || []).length)
                                    .arg((root.outcome.selectedTracks || []).map(track => track.name).join(", "))
                    }
                    BodyText {
                        visible: root.planned ? root.mixed : root.outcome.path === "Render"
                        text: qsTr("Other tracks will not be included: %1")
                            .arg((root.outcome.excludedTracks || []).map(track => track.name).join(", ") || qsTr("none"))
                    }
                    BodyText {
                        visible: root.planned || root.outcome.path === "Render"
                        text: qsTr("Original source recordings remain unchanged.")
                    }
                    BodyText {
                        visible: root.outcome.replaceExisting === true
                        color: ExoTheme.warningText
                        text: qsTr("The existing destination file will be replaced.")
                    }
                }
            }

            // Job and result.
            Banner {
                objectName: "editExportDoneBanner"
                visible: root.jobState === EditExportAdapter.Done
                text: qsTr("Export complete.")
            }
            Banner {
                objectName: "editExportErrorBanner"
                visible: root.jobState === EditExportAdapter.Failed
                tint: ExoTheme.errorText
                glyph: ExoGlyph.Warning
                text: root.exporter.errorText
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: !root.settings && root.exporter.outputFileName.length > 0
                spacing: 2
                FieldLabel { text: qsTr("File") }
                Label {
                    objectName: "editExportFileName"
                    Layout.fillWidth: true
                    text: root.exporter.outputFileName
                    elide: Text.ElideMiddle
                    color: ExoTheme.text
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontBody
                }
                FieldLabel { text: qsTr("Folder"); Layout.topMargin: ExoTheme.spacingXs }
                Label {
                    Layout.fillWidth: true
                    text: root.exporter.outputFolder
                    elide: Text.ElideMiddle
                    color: root.jobState === EditExportAdapter.Failed && root.exporter.destinationFailure ? ExoTheme.errorText : ExoTheme.text
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontSecondary
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.working
                spacing: ExoTheme.spacingXs
                ExoProgressBar {
                    objectName: "editExportProgressBar"
                    Layout.fillWidth: true
                    value: root.exporter.progressPercent / 100
                }
                Label {
                    objectName: "editExportProgress"
                    Layout.fillWidth: true
                    text: root.jobState === EditExportAdapter.Preparing ? qsTr("Preparing export…")
                        : root.jobState === EditExportAdapter.Cancelling ? qsTr("Cancelling…")
                        : qsTr("%1%").arg(root.exporter.progressPercent)
                    color: ExoTheme.text
                    font.family: ExoTheme.monoFamily
                    font.pixelSize: ExoTheme.fontSecondary
                    Accessible.name: qsTr("Exporting %1%").arg(root.exporter.progressPercent)
                }
                BodyText { text: qsTr("You can keep editing. Changes made now are not part of this export.") }
            }
            BodyText {
                objectName: "editExportOutdated"
                visible: root.exporter.resultOutdated
                text: qsTr("The edit changed after this export. New export uses the current state.")
            }
        }
    }

    // Fixed footer with exactly one primary action per state.
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: ExoTheme.line
    }
    GridLayout {
        Layout.fillWidth: true
        Layout.topMargin: 10
        columns: !secondaryAction.visible || primaryAction.implicitWidth + secondaryAction.implicitWidth + columnSpacing <= root.width ? 2 : 1
        columnSpacing: ExoTheme.spacingSm
        rowSpacing: ExoTheme.spacingSm
        ExoButton {
            id: secondaryAction
            objectName: root.jobState === EditExportAdapter.Review ? "editExportReviewCancel" : "editExportSecondary"
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            visible: text.length > 0
            text: root.jobState === EditExportAdapter.Review ? qsTr("Back")
                : root.jobState === EditExportAdapter.Done ? qsTr("New export")
                : root.jobState === EditExportAdapter.Failed ? qsTr("Back to settings") : ""
            onClicked: {
                if (root.jobState === EditExportAdapter.Review)
                    root.exporter.discardPreparedExport();
                else
                    root.exporter.reset();
                Qt.callLater(root.focusFirst);
            }
        }
        ExoButton {
            id: primaryAction
            objectName: root.jobState === EditExportAdapter.Review ? "editExportReviewConfirm"
                : root.working ? "editCancelExport" : "editExportPrimary"
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            tone: root.working ? "neutral" : "primary"
            enabled: root.jobState === EditExportAdapter.Cancelling ? false
                : root.jobState === EditExportAdapter.Options ? root.session.durationUs > 0 : true
            text: {
                switch (root.jobState) {
                case EditExportAdapter.Options: return qsTr("Export…");
                case EditExportAdapter.Review:
                    return root.planned ? qsTr("Export")
                        : root.outcome.path === "Render" ? qsTr("Render with selected tracks") : qsTr("Start copy");
                case EditExportAdapter.Done: return qsTr("Show in folder");
                case EditExportAdapter.Failed:
                    return root.exporter.destinationFailure ? qsTr("Choose another folder…") : qsTr("Try again");
                default: return qsTr("Cancel export");
                }
            }
            onClicked: {
                switch (root.jobState) {
                case EditExportAdapter.Options: root.exporter.chooseDestination(); break;
                case EditExportAdapter.Review: root.exporter.confirmPreparedExport(); break;
                case EditExportAdapter.Done: root.exporter.revealFile(); break;
                case EditExportAdapter.Failed:
                    if (root.exporter.destinationFailure) retryFolder.open();
                    else root.exporter.retry();
                    break;
                default: root.exporter.cancel(); break;
                }
            }
        }
    }
    FolderDialog {
        id: retryFolder
        title: qsTr("Choose another folder")
        onAccepted: root.exporter.retryInFolder(selectedFolder)
    }
}
