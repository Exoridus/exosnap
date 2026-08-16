pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Diagnostics nav area. A calm Simple default — verdict band, responsive readiness
// tiles, worst-first cards, one bundled tip chip — with an Expert toggle that
// reveals the flat taxonomy beneath the SAME band and tiles.
//
// The page renders; it decides nothing. Verdict wording, tier→card/tip split, tile
// text and pipeline health all arrive already resolved from DiagnosticsAdapter.
// The first probe (volume query, output-path write test, self-test) starts here,
// the first time the page becomes visible, and runs on a worker thread.
Item {
    id: root

    required property DiagnosticsAdapter diagnostics
    // Detected adapters and their encoder capabilities. Observed facts about the
    // machine, so they belong here rather than under a navigation destination of
    // their own — see DeviceCapabilityPanel.
    required property DeviceAdapter device

    // The tile grid reflows on the tiles' own minimum width rather than on window
    // thresholds, so it stays right inside a narrow column too. Free in QML; there
    // is no C++ column policy to keep in sync any more.
    readonly property int tileColumns: ExoTheme.gridColumns(root.contentWidth, 210, ExoTheme.spacingMd, 4)

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
    // page opens on, and the collapsed hardware-capability section that carries
    // the per-GPU adapter cards and the capability matrix. Everything else on
    // this page is either always visible or Expert-only taxonomy that the Expert
    // toggle already governs.
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
        // Hardware capabilities is collapsed by default outside Expert, and a
        // scroll that stops at a closed header has not revealed the section any
        // more than not scrolling would have. Assigning `expanded` is exactly
        // what pressing the header does.
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

        // ── Page header: identity, the support-bundle export, the Expert toggle ──
        //
        // A page title on the same rung, the same axis and the same inset as
        // Settings and Device. It was a 12 px mono kicker under a full-window
        // hairline, on an axis 24 px further in than its own cards — three
        // different page-header treatments across six pages, and one of them
        // misaligned with its own content.
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

            Label {
                text: root.diagnostics.expertMode ? qsTr("Expert — full taxonomy") : qsTr("Simple")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.textMuted
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignBaseline
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                }
            }

            // Chromed, not quiet: this one writes a file to disk and it must not
            // read like the muted mode label two items to its right.
            ExoButton {
                text: root.diagnostics.bundleBusy ? qsTr("Creating…") : qsTr("Create support bundle")
                visible: root.diagnostics.expertMode
                enabled: !root.diagnostics.bundleBusy
                Accessible.description: qsTr("Create a diagnostic package to share with support")
                onClicked: bundleDialog.open()
            }

            Label {
                text: qsTr("Expert mode")
                textFormat: Text.PlainText
                color: root.diagnostics.expertMode ? ExoTheme.accent : ExoTheme.textSecondary
                Layout.alignment: Qt.AlignVCenter
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                }
            }

            ExoSwitch {
                checked: root.diagnostics.expertMode
                Accessible.name: qsTr("Expert mode")
                onToggledByUser: function (value) {
                    root.diagnostics.expertMode = value;
                }
            }
        }

        ExoScrollView {
            id: scroll

            contentWidth: availableWidth
            clip: true
            // The vertical scroll bar overlays content, so its gutter is reserved
            // unconditionally — this page always scrolls in Expert.
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

                        ColumnLayout {
                            spacing: ExoTheme.spacingSm
                            Layout.alignment: Qt.AlignVCenter

                            Label {
                                text: root.diagnostics.lastCheckText
                                textFormat: Text.PlainText
                                horizontalAlignment: Text.AlignRight
                                color: ExoTheme.textDim
                                Layout.alignment: Qt.AlignRight
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontCaption
                                }
                            }

                            // The one action the verdict band offers, so it takes
                            // the primary rung: accent-filled and on the taller
                            // CTA height, matching the Widgets reference. Left
                            // neutral it rendered as a text run beside the
                            // timestamp above it.
                            ExoButton {
                                text: root.diagnostics.checking ? qsTr("Checking…") : qsTr("Run Check")
                                tone: "primary"
                                enabled: !root.diagnostics.checking
                                Layout.alignment: Qt.AlignRight
                                onClicked: root.diagnostics.runCheck()
                            }
                        }
                    }
                }

                // ── Readiness tiles ─────────────────────────────────────────────
                GridLayout {
                    columns: root.tileColumns
                    columnSpacing: ExoTheme.spacingMd
                    rowSpacing: ExoTheme.spacingMd
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
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                        }
                    }
                }

                // ── Worst-first cards (shared: Simple + Expert) ─────────────────
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
                    defaultOpen: root.diagnostics.expertMode
                    Layout.fillWidth: true
                    onApplyFixRequested: function (id) {
                        root.diagnostics.applyFix(id);
                    }
                    onAssistedFixRequested: function (id) {
                        root.diagnostics.openAssistedFix(id);
                    }
                }

                // ── Hardware capabilities ───────────────────────────────────────
                //
                // Available in Simple as well as Expert: this used to be a whole
                // navigation destination, and burying it behind the Expert toggle
                // would have removed reachable functionality rather than moved it.
                // Collapsed by default so a healthy page stays short, and so the
                // DXGI enumeration + NVENC probe behind it only runs when asked
                // for — expanded straight away in Expert, where the rest of the
                // technical taxonomy is already open.
                ExoDisclosure {
                    id: hardwareCapabilitiesSection

                    // No subtitle: the panel opens on DeviceAdapter's own summary
                    // line, which says the same thing with the real adapter name
                    // in it. Two explanatory paragraphs stacked on top of each
                    // other read as one of them being unread.
                    title: qsTr("Hardware capabilities")
                    expanded: root.diagnostics.expertMode
                    Layout.fillWidth: true

                    body: Component {
                        DeviceCapabilityPanel {
                            device: root.device
                        }
                    }
                }

                // ── Expert-only taxonomy ────────────────────────────────────────
                ColumnLayout {
                    spacing: ExoTheme.spacingLg
                    visible: root.diagnostics.expertMode
                    Layout.fillWidth: true

                    DiagnosticsSectionHeader {
                        title: qsTr("ENVIRONMENT")
                        Layout.fillWidth: true
                    }

                    ExoKeyValueTable {
                        rows: root.diagnostics.environmentRows
                        Layout.fillWidth: true
                    }

                    ExoDisclosure {
                        title: qsTr("2 · Pre-flight & Readiness")
                        subtitle: qsTr("Tier-1 gates the start · Tier-3 informs. Self-test validates core pipeline components.")
                        expanded: true
                        Layout.fillWidth: true

                        body: Component {
                            ColumnLayout {
                                spacing: ExoTheme.spacingSm

                                RowLayout {
                                    spacing: ExoTheme.spacingMd
                                    Layout.fillWidth: true

                                    Label {
                                        text: root.diagnostics.selfTestStatus
                                        textFormat: Text.PlainText
                                        color: ExoTheme.textSecondary
                                        Layout.fillWidth: true
                                        font {
                                            family: ExoTheme.sansFamily
                                            pixelSize: ExoTheme.fontSecondary
                                        }
                                    }

                                    ExoButton {
                                        text: qsTr("Run Self-Test")
                                        enabled: !root.diagnostics.checking
                                        quiet: true
                                        onClicked: root.diagnostics.runCheck()
                                    }
                                }

                                Label {
                                    text: qsTr("Run a system check or click Run Self-Test.")
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WordWrap
                                    visible: root.diagnostics.selfTestRows.length === 0
                                    color: ExoTheme.textMuted
                                    Layout.fillWidth: true
                                    font {
                                        family: ExoTheme.sansFamily
                                        pixelSize: ExoTheme.fontCaption
                                    }
                                }

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

                    ExoDisclosure {
                        title: qsTr("3 · Live pipeline")
                        subtitle: qsTr("Low-overhead runtime metrics for the active recording (~5×/s). Unmeasured values are shown as Unavailable, never zero.")
                        expanded: true
                        Layout.fillWidth: true

                        body: Component {
                            ColumnLayout {
                                spacing: ExoTheme.spacingSm

                                Label {
                                    text: root.diagnostics.pipelineLive
                                        ? qsTr("Live — measured from the running recording.")
                                        : qsTr("Idle — stage timings appear once a recording starts.")
                                    textFormat: Text.PlainText
                                    color: ExoTheme.textMuted
                                    Layout.fillWidth: true
                                    font {
                                        family: ExoTheme.sansFamily
                                        pixelSize: ExoTheme.fontCaption
                                    }
                                }

                                ExoPipelineFlow {
                                    stages: root.diagnostics.pipelineStages
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }

                    ExoDisclosure {
                        title: qsTr("4 · Post-flight & Review")
                        subtitle: qsTr("After Stop: drop-%, max drift, achieved vs target and file validity, then a bridge to the Edit overlay.")
                        Layout.fillWidth: true

                        body: Component {
                            ColumnLayout {
                                spacing: ExoTheme.spacingSm

                                Label {
                                    text: qsTr("The report card appears in the Edit view's Review step after a recording finishes.")
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WordWrap
                                    color: ExoTheme.textMuted
                                    Layout.fillWidth: true
                                    font {
                                        family: ExoTheme.sansFamily
                                        pixelSize: ExoTheme.fontCaption
                                    }
                                }

                                ExoButton {
                                    text: qsTr("Open last report")
                                    enabled: root.diagnostics.hasLastRecording
                                    quiet: true
                                    Layout.alignment: Qt.AlignLeft
                                    onClicked: root.diagnostics.openLastReport()
                                }
                            }
                        }
                    }

                    ExoDisclosure {
                        title: qsTr("Active configuration")
                        subtitle: qsTr("Recording settings as currently configured in the app.")
                        Layout.fillWidth: true

                        body: Component {
                            ExoKeyValueTable {
                                rows: root.diagnostics.configRows
                            }
                        }
                    }

                    DiagnosticsSectionHeader {
                        title: qsTr("ELEVATED DIAGNOSTICS")
                        meta: qsTr("Opt-in · relaunch as admin")
                        Layout.fillWidth: true
                    }

                    DiagnosticsRowCard {
                        Layout.fillWidth: true

                        Label {
                            text: root.diagnostics.elevated
                                ? qsTr("Running elevated — PresentMon ETW present diagnostics are available.")
                                : qsTr("Running standard — present-path and DPC/ISR measurements need an elevated relaunch.")
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

                        ExoBadge {
                            text: root.diagnostics.elevated ? qsTr("Elevated") : qsTr("Standard")
                            tone: root.diagnostics.elevated ? "pass" : "neutral"
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }

                    // ── Logs redirect (Expert; the Simple view stays calm) ──────
                    DiagnosticsRowCard {
                        Layout.fillWidth: true

                        ColumnLayout {
                            spacing: 2
                            Layout.fillWidth: true

                            Label {
                                text: qsTr("Application Logs")
                                textFormat: Text.PlainText
                                color: ExoTheme.text
                                Layout.fillWidth: true
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontBody
                                    weight: Font.DemiBold
                                }
                            }

                            Label {
                                text: qsTr("Need the raw event stream behind these checks? Open the Logs page.")
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                color: ExoTheme.textMuted
                                Layout.fillWidth: true
                                Layout.minimumHeight: 16
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontCaption
                                }
                            }
                        }

                        ExoButton {
                            text: qsTr("Open Logs Page")
                            quiet: true
                            Layout.alignment: Qt.AlignVCenter
                            onClicked: root.diagnostics.openLogs()
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
