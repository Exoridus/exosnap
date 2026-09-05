pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Diagnostics nav area. One layout, ordered by what the recorder is doing:
// idle it answers "may I start", recording it answers "how is it going", and
// after Stop it answers "how did it go". Expert mode is a Settings-only control
// and this page does not read it.
//
// The page renders; it decides nothing. Verdict wording, tier→card/tip split,
// tile text, value tint, the session ledger and the last-session facts all
// arrive already resolved from DiagnosticsAdapter. The first probe (volume
// query, output-path write test, self-test) starts here, the first time the page
// becomes visible, and runs on a worker thread.
Item {
    id: root

    required property DiagnosticsAdapter diagnostics
    // Detected adapters and their encoder capabilities. Observed facts about the
    // machine, so they belong here rather than under a navigation destination of
    // their own — see DeviceCapabilityPanel.
    required property DeviceAdapter device

    // Tile rows are always full: four columns or two, never a ragged three. A
    // row with a gap in it reads as something failing to load.
    readonly property int tileColumns: ExoTheme.tileColumns(root.contentWidth, 210, ExoTheme.spacingMd)

    // The capped, centred reading column. Shared by the header above the scroll
    // view and the content inside it, so the page identity is not pinned to the
    // window edge while every card under it starts further in.
    //
    // Same formula as Settings and Device — page padding on both sides minus the
    // reserved scrollbar gutter. It used to subtract only the gutter, which put
    // this page's capped column on an axis 12 px off from theirs at every window
    // width.
    readonly property real contentBox: Math.max(0, root.width - 2 * ExoTheme.pagePadding - ExoTheme.spacingLg)
    readonly property real contentWidth: Math.min(root.contentBox, ExoTheme.contentMaxWidth)
    readonly property real sideInset: Math.max(0, (root.contentBox - root.contentWidth) / 2)

    signal navigateToLogsRequested()
    signal navigateToSettingsRequested()

    objectName: "quickDiagnosticsPage"

    // ── Automation targets (protocol 2 ui.reveal) ────────────────────────────
    //
    // Two, because two are addressable product landmarks: the verdict band the
    // page opens on, and the collapsed hardware-capability reference row that
    // carries the per-GPU adapter cards and the capability matrix. Everything
    // else on this page is either always visible or reached by expanding one of
    // these.
    readonly property var automationTargets: ({
        "verdict": verdictBand,
        "hardwareCapabilities": hardwareCapabilitiesSection
    })

    // -1 = no such target, 0 = a real target that did not reach the viewport,
    // 1 = revealed. Same three-way answer as SettingsPage, for the same reason.
    function revealAutomationTarget(name: string): int {
        const section = root.automationTargets[name];
        if (section === undefined || section === null)
            return -1;
        // Hardware capabilities is collapsed by default, and a scroll that stops
        // at a closed header has not revealed the section any more than not
        // scrolling would have. Assigning `expanded` is exactly what pressing
        // the header does.
        if (section === hardwareCapabilitiesSection)
            hardwareCapabilitiesSection.expanded = true;
        return scroll.revealItem(section) ? 1 : 0;
    }

    function scrollAutomationHome(): bool {
        return scroll.scrollToHome();
    }

    function scrollAutomationEnd(): bool {
        return scroll.scrollToEnd();
    }

    onVisibleChanged: {
        if (root.visible) {
            root.diagnostics.ensureChecked();
        }
    }

    Component.onCompleted: {
        if (root.visible) {
            root.diagnostics.ensureChecked();
        }
    }

    Connections {
        target: root.diagnostics

        function onFixConfirmRequested(fixId: string, label: string, changesSummary: string): void {
            confirmDialog.pendingFixId = fixId;
            confirmDialog.title = label === "" ? qsTr("Apply fix") : label;
            confirmDialog.bodyText = changesSummary === ""
                ? qsTr("Apply this fix to your recording settings?")
                : changesSummary;
            confirmDialog.open();
        }

        function onNavigateToLogsRequested(): void {
            root.navigateToLogsRequested();
        }

        function onNavigateToSettingsRequested(): void {
            root.navigateToSettingsRequested();
        }
    }

    ExoConfirmDialog {
        id: confirmDialog

        property string pendingFixId: ""

        title: qsTr("Apply fix")
        onAccepted: root.diagnostics.acceptFix(confirmDialog.pendingFixId)
    }

    FileDialog {
        id: bundleDialog

        title: qsTr("Save support bundle")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Zip archives (*.zip)")]
        defaultSuffix: "zip"
        onAccepted: root.diagnostics.createSupportBundle(bundleDialog.selectedFile)
    }

    ColumnLayout {
        spacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            margins: ExoTheme.pagePadding
        }

        // ── Page header: identity and the in-depth switch ─────────────────────
        //
        // A page title on the same rung, the same axis and the same inset as
        // Settings and Device. The one header action is the switch that decides
        // how deep the live measurements go; creating a support bundle is a
        // reference row at the bottom, where the rest of the export-shaped
        // affordances live.
        RowLayout {
            spacing: ExoTheme.spacingMd
            Layout.fillWidth: true
            Layout.bottomMargin: ExoTheme.spacingXs
            Layout.leftMargin: root.sideInset
            Layout.rightMargin: root.sideInset + ExoTheme.spacingLg

            Label {
                text: qsTr("Diagnostics")
                textFormat: Text.PlainText
                color: ExoTheme.text
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontPageTitle
                    weight: Font.DemiBold
                }
            }

            Item {
                Layout.fillWidth: true
            }

            // Its own fixed gap rather than the header row's general item
            // spacing: a label and the one switch it names read as a single
            // control. Same value in SettingsPresetBar.qml, which carries the
            // twin of this switch.
            RowLayout {
                spacing: ExoTheme.spacingSm
                Layout.alignment: Qt.AlignVCenter
                // Dimmed rather than hidden while recording: the gate is a fact
                // about this session, and a control that vanishes reads as one
                // the product forgot about.
                opacity: root.diagnostics.inDepthAvailable ? 1.0 : 0.45

                ColumnLayout {
                    spacing: 2
                    Layout.alignment: Qt.AlignVCenter

                    Label {
                        text: qsTr("In-depth diagnostics")
                        textFormat: Text.PlainText
                        horizontalAlignment: Text.AlignRight
                        color: ExoTheme.text
                        Layout.alignment: Qt.AlignRight
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontBody
                        }
                    }

                    Label {
                        objectName: "diagnosticsInDepthState"
                        text: root.diagnostics.inDepthStateText
                        textFormat: Text.PlainText
                        horizontalAlignment: Text.AlignRight
                        color: ExoTheme.textMuted
                        Layout.alignment: Qt.AlignRight
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontCaption
                        }
                    }
                }

                ExoSwitch {
                    objectName: "diagnosticsInDepthSwitch"
                    checked: root.diagnostics.inDepthEnabled
                    enabled: root.diagnostics.inDepthAvailable
                    Accessible.name: qsTr("In-depth diagnostics")
                    Layout.alignment: Qt.AlignVCenter
                    onToggledByUser: function (value) {
                        root.diagnostics.inDepthEnabled = value;
                    }
                }
            }
        }

        ExoScrollView {
            id: scroll

            contentWidth: availableWidth
            clip: true
            // The vertical scroll bar overlays content, so its gutter is reserved
            // unconditionally — this page always scrolls.
            rightPadding: ExoTheme.spacingLg
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                spacing: ExoTheme.spacingLg
                // Capped and centred: a healthy run is a short page, and stretching
                // six tiles across a wide desktop reads as something missing.
                width: root.contentWidth
                x: root.sideInset

                // ── Verdict band ────────────────────────────────────────────────
                //
                // No action of its own. The page probes on first visit, every
                // 10 s and on every settings change, and the stamp says so, so a
                // Run check button would only offer to do again what is already
                // being done.
                Rectangle {
                    id: verdictBand

                    readonly property color verdictColor: root.diagnostics.verdictState === "blocked" ? ExoTheme.error
                                                        : root.diagnostics.verdictState === "warn" ? ExoTheme.warning
                                                        : root.diagnostics.verdictState === "ready" ? ExoTheme.success
                                                        : ExoTheme.textMuted
                    // The glyph is what says which verdict this is; the two
                    // borders around it only mark the band.
                    readonly property color verdictInk: root.diagnostics.verdictState === "blocked" ? ExoTheme.errorText
                                                      : root.diagnostics.verdictState === "warn" ? ExoTheme.warningText
                                                      : root.diagnostics.verdictState === "ready" ? ExoTheme.successText
                                                      : ExoTheme.textMuted

                    implicitHeight: verdictRow.implicitHeight + 2 * ExoTheme.spacingLg
                    color: root.diagnostics.verdictState === "blocked" ? ExoTheme.errorSurface
                         : root.diagnostics.verdictState === "warn" ? ExoTheme.warningSurface
                         : ExoTheme.surface
                    border.width: 1
                    border.color: root.diagnostics.verdictState === "neutral" ? ExoTheme.line : verdictBand.verdictColor
                    radius: ExoTheme.radiusLg
                    Layout.fillWidth: true

                    RowLayout {
                        id: verdictRow

                        spacing: ExoTheme.spacingMd
                        anchors {
                            fill: parent
                            margins: ExoTheme.spacingLg
                        }

                        Rectangle {
                            color: ExoTheme.surfaceRaised
                            border.width: 1
                            border.color: verdictBand.verdictColor
                            radius: ExoTheme.radiusMd
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            Layout.alignment: Qt.AlignVCenter

                            ExoGlyph {
                                anchors.centerIn: parent
                                kind: root.diagnostics.verdictState === "blocked" ? ExoGlyph.Close
                                      : root.diagnostics.verdictState === "warn" ? ExoGlyph.Warning
                                      : root.diagnostics.verdictState === "ready" ? ExoGlyph.Check
                                                                                  : ExoGlyph.Info
                                color: verdictBand.verdictInk
                                strokeWidth: 1.8
                                width: 20
                                height: 20
                            }
                        }

                        ColumnLayout {
                            spacing: ExoTheme.spacingXs
                            Layout.fillWidth: true

                            Label {
                                text: root.diagnostics.verdictHeadline
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                color: ExoTheme.text
                                Layout.fillWidth: true
                                Layout.minimumHeight: 22
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontSectionTitle
                                    weight: Font.DemiBold
                                }
                            }

                            Label {
                                text: root.diagnostics.verdictSubline
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                color: ExoTheme.textMuted
                                Layout.fillWidth: true
                                Layout.minimumHeight: 17
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontSecondary
                                }
                            }
                        }

                        Label {
                            text: root.diagnostics.lastCheckText
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignRight
                            color: ExoTheme.textDim
                            Layout.maximumWidth: 220
                            // The row gathers implicitHeight before it constrains
                            // the width, so an unconstrained wrapping label is
                            // sized for one line and clips the rest -- and the
                            // recheck policy the band exists to state is what
                            // gets clipped. Three caption lines at 220 px.
                            Layout.minimumHeight: 3 * 16
                            Layout.alignment: Qt.AlignVCenter
                            font {
                                family: ExoTheme.sansFamily
                                pixelSize: ExoTheme.fontCaption
                            }
                        }
                    }
                }

                // ── Live pipeline ───────────────────────────────────────────────
                //
                // Present only while something is recording. Readiness answers
                // "may I start", which stops being the question the moment a
                // recording is running, so the readiness tiles below are hidden
                // for as long as this section is up.
                //
                // Every value comes from diagnostics::BuildLiveTiles over the
                // engine's own snapshot. The page classifies nothing: a tile's
                // tone IS the engine's health plus its bottleneck attribution,
                // and the tint of a single number is the verdict of the check
                // that owns it.
                ColumnLayout {
                    spacing: ExoTheme.spacingSm
                    visible: root.diagnostics.recording
                    Layout.fillWidth: true

                    DiagnosticsSectionHeader {
                        title: qsTr("LIVE PIPELINE")
                        meta: qsTr("measured from the running recording")
                        Layout.fillWidth: true
                    }

                    ExoPipelineFlow {
                        stages: root.diagnostics.pipelineStages
                        Layout.fillWidth: true
                    }

                    GridLayout {
                        columns: root.tileColumns
                        columnSpacing: ExoTheme.spacingMd
                        rowSpacing: ExoTheme.spacingMd
                        Layout.fillWidth: true

                        Repeater {
                            model: root.diagnostics.liveTiles

                            ExoStatusTile {
                                id: liveTile

                                required property var modelData

                                title: liveTile.modelData.title
                                value: liveTile.modelData.value
                                sub: liveTile.modelData.sub
                                subTinted: liveTile.modelData.subTinted
                                subTone: liveTile.modelData.subTone
                                detail: liveTile.modelData.detail
                                tone: liveTile.modelData.tone
                                valueTone: liveTile.modelData.valueTone
                                series: liveTile.modelData.series
                                budget: liveTile.modelData.budget
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                            }
                        }
                    }
                }

                // ── Observed in this session ────────────────────────────────────
                //
                // The session ledger: what was measured, when, and how often. An
                // entry never leaves before Stop, because "it happened" is the
                // answer the owner of the finished file needs.
                ColumnLayout {
                    spacing: ExoTheme.spacingSm
                    visible: root.diagnostics.recording && root.diagnostics.ledgerCount > 0
                    Layout.fillWidth: true

                    DiagnosticsSectionHeader {
                        title: qsTr("OBSERVED IN THIS SESSION")
                        meta: qsTr("stays until Stop · order = first seen")
                        Layout.fillWidth: true
                    }

                    Repeater {
                        model: root.diagnostics.ledger

                        ExoLedgerCard {
                            id: ledgerCard

                            // Roles arrive through the injected `model` object
                            // rather than as required properties: several role
                            // names collide with this component's own.
                            required property var model

                            entryId: ledgerCard.model.entryId
                            title: ledgerCard.model.title
                            summary: ledgerCard.model.summary
                            active: ledgerCard.model.active
                            count: ledgerCard.model.count
                            firstSeenText: ledgerCard.model.firstSeenText
                            lastSeenText: ledgerCard.model.lastSeenText
                            worstText: ledgerCard.model.worstText
                            budgetText: ledgerCard.model.budgetText
                            totalActiveText: ledgerCard.model.totalActiveText
                            logExcerpt: ledgerCard.model.logExcerpt
                            occurrences: ledgerCard.model.occurrences
                            // Firing right now is the card; gone quiet is the row.
                            expanded: ledgerCard.model.active
                            Layout.fillWidth: true

                            onShowInLogRequested: function (entryId) {
                                root.diagnostics.showInLog(entryId);
                            }
                            onOpenAtRequested: function (startMs) {
                                root.diagnostics.openEditAt(startMs);
                            }
                        }
                    }
                }

                // ── Last session ────────────────────────────────────────────────
                //
                // Until the next recording starts. A recording is judged by what
                // happened to it, which is the frozen ledger and the timeline
                // under these four facts.
                ColumnLayout {
                    spacing: ExoTheme.spacingSm
                    visible: root.diagnostics.hasLastSession && !root.diagnostics.recording
                    Layout.fillWidth: true

                    DiagnosticsSectionHeader {
                        title: qsTr("LAST SESSION")
                        meta: root.diagnostics.lastSession.startedAtText === ""
                            ? root.diagnostics.lastSession.durationText
                            : qsTr("%1 – %2 · %3").arg(root.diagnostics.lastSession.startedAtText)
                                                  .arg(root.diagnostics.lastSession.endedAtText)
                                                  .arg(root.diagnostics.lastSession.durationText)
                        Layout.fillWidth: true
                    }

                    // Created only once there is a session to describe. The
                    // enclosing column is merely invisible before that, and a
                    // card built against an empty map binds every one of its
                    // lookups to undefined -- including the timeline's required
                    // durationMs.
                    Loader {
                        active: root.diagnostics.hasLastSession
                        Layout.fillWidth: true

                        sourceComponent: ExoLastSessionCard {
                            session: root.diagnostics.lastSession
                            columns: root.tileColumns

                            onShowInFolderRequested: root.diagnostics.openLastSessionFolder()
                            onOpenEditRequested: root.diagnostics.openEditAt(0)
                            onOpenEditAtRequested: function (positionMs) {
                                root.diagnostics.openEditAt(positionMs);
                            }
                            onViewLogRequested: root.diagnostics.openLogs()
                            onShowInLogRequested: function (entryId) {
                                root.diagnostics.showInLog(entryId);
                            }
                        }
                    }
                }

                // ── Readiness tiles ─────────────────────────────────────────────
                GridLayout {
                    columns: root.tileColumns
                    columnSpacing: ExoTheme.spacingMd
                    rowSpacing: ExoTheme.spacingMd
                    visible: !root.diagnostics.recording
                    Layout.fillWidth: true

                    Repeater {
                        model: root.diagnostics.tiles

                        ExoStatusTile {
                            id: tile

                            required property var modelData

                            title: tile.modelData.title
                            value: tile.modelData.value
                            sub: tile.modelData.sub
                            tone: tile.modelData.tone
                            showOkGlyph: tile.modelData.showOkGlyph
                            hasUsageBar: tile.modelData.hasUsageBar
                            usagePercent: tile.modelData.usagePercent
                            headBadge: tile.modelData.headBadge
                            chips: tile.modelData.chips
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                        }
                    }
                }

                // ── Worst-first cards ───────────────────────────────────────────
                //
                // Tier-1 blockers, and Tier-2 measured problems while nothing is
                // recording. The adapter drops Tier-2 cards for the duration of a
                // session, where the ledger above tells the same finding better.
                ColumnLayout {
                    spacing: ExoTheme.spacingSm
                    visible: root.diagnostics.hasIssues
                    Layout.fillWidth: true

                    Repeater {
                        model: root.diagnostics.issues

                        ExoIssueCard {
                            id: card

                            // Roles arrive through the injected `model` object rather
                            // than as required properties: several role names collide
                            // with this component's own property names.
                            required property var model

                            issueId: card.model.issueId
                            tone: card.model.tone
                            title: card.model.title
                            summary: card.model.summary
                            why: card.model.why
                            measured: card.model.measured
                            logExcerpt: card.model.logExcerpt
                            needsElevation: card.model.needsElevation
                            hasEvidence: card.model.hasEvidence
                            hasFix: card.model.hasFix
                            fixId: card.model.fixId
                            fixLabel: card.model.fixLabel
                            fixSafety: card.model.fixSafety
                            Layout.fillWidth: true
                            onApplyFixRequested: function (id) {
                                root.diagnostics.applyFix(id);
                            }
                            onAssistedFixRequested: function (id) {
                                root.diagnostics.openAssistedFix(id);
                            }
                        }
                    }
                }

                ExoTipChip {
                    tips: root.diagnostics.tips
                    Layout.fillWidth: true
                    onApplyFixRequested: function (id) {
                        root.diagnostics.applyFix(id);
                    }
                    onAssistedFixRequested: function (id) {
                        root.diagnostics.openAssistedFix(id);
                    }
                }

                // ── Reference ───────────────────────────────────────────────────
                //
                // The same four collapsed rows in every state, each stating its
                // answer in the header so the section only has to be opened for
                // the detail behind it.
                ColumnLayout {
                    spacing: ExoTheme.spacingSm
                    Layout.fillWidth: true

                    DiagnosticsSectionHeader {
                        title: qsTr("REFERENCE")
                        meta: qsTr("the same in every state")
                        Layout.fillWidth: true
                    }

                    ExoReferenceRow {
                        title: qsTr("Self-test")
                        // "Self-test: Capture" repeated five times is the section
                        // title five times over; the row titles are stripped back
                        // to what each one actually checked.
                        summary: root.diagnostics.selfTestRows.length === 0
                            ? qsTr("Not run yet")
                            : root.diagnostics.selfTestStatus.replace(/^[^:]*:\s*/, "").toUpperCase() + " · "
                              + root.diagnostics.selfTestRows.map(function (row) {
                                    return row.title.replace(/^[^:]*:\s*/, "").toLowerCase();
                                }).join(", ")
                        Layout.fillWidth: true

                        trailing: Component {
                            ExoButton {
                                text: qsTr("Run again")
                                leadingGlyph: ExoGlyph.Run
                                quiet: true
                                compact: true
                                enabled: !root.diagnostics.checking
                                onClicked: root.diagnostics.runCheck()
                            }
                        }

                        body: Component {
                            ColumnLayout {
                                spacing: ExoTheme.spacingSm

                                Repeater {
                                    model: root.diagnostics.selfTestRows

                                    DiagnosticsSelfTestRow {
                                        id: selfTestRow

                                        required property var modelData

                                        title: selfTestRow.modelData.title
                                        statusText: selfTestRow.modelData.statusText
                                        detail: selfTestRow.modelData.detail
                                        tone: selfTestRow.modelData.tone
                                        notRun: selfTestRow.modelData.notRun
                                        Layout.fillWidth: true
                                    }
                                }
                            }
                        }
                    }

                    // This used to be a whole navigation destination. Collapsed by
                    // default so a healthy page stays short, and so the DXGI
                    // enumeration + NVENC probe behind it only runs when asked for.
                    ExoReferenceRow {
                        id: hardwareCapabilitiesSection

                        title: qsTr("Hardware capabilities")
                        summary: root.device.selectedTitle === ""
                            ? qsTr("Not scanned yet")
                            : root.device.selectedTitle + " · " + root.device.selectedSubtitle
                        Layout.fillWidth: true

                        trailing: Component {
                            ExoButton {
                                text: root.device.scanning ? qsTr("Scanning…") : qsTr("Rescan")
                                quiet: true
                                compact: true
                                enabled: !root.device.scanning
                                onClicked: root.device.rescan()
                            }
                        }

                        body: Component {
                            DeviceCapabilityPanel {
                                device: root.device
                            }
                        }
                    }

                    ExoReferenceRow {
                        title: qsTr("Environment & configuration")
                        summary: root.diagnostics.environmentRows.length === 0
                            ? qsTr("Measured on the first check")
                            : root.diagnostics.environmentRows.slice(0, 3).map(function (row) {
                                  return row.value;
                              }).join(" · ")
                        Layout.fillWidth: true

                        body: Component {
                            ColumnLayout {
                                spacing: ExoTheme.spacingMd

                                ExoKeyValueTable {
                                    rows: root.diagnostics.environmentRows
                                    Layout.fillWidth: true
                                }

                                ExoKeyValueTable {
                                    rows: root.diagnostics.configRows
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }

                    ExoReferenceRow {
                        title: qsTr("Support bundle")
                        summary: qsTr("Logs, configuration, self-test and the last session report, as one file to share")
                        Layout.fillWidth: true

                        trailing: Component {
                            ExoButton {
                                text: root.diagnostics.bundleBusy ? qsTr("Creating…") : qsTr("Create")
                                leadingGlyph: ExoGlyph.Folder
                                quiet: true
                                compact: true
                                enabled: !root.diagnostics.bundleBusy
                                Accessible.description: qsTr("Create a diagnostic package to share with support")
                                onClicked: bundleDialog.open()
                            }
                        }

                        body: Component {
                            Label {
                                text: qsTr("The bundle is written where you choose and never leaves this machine on its own.")
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                color: ExoTheme.textMuted
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontCaption
                                }
                            }
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                    Layout.minimumHeight: ExoTheme.spacingXl
                }
            }
        }
    }
}
