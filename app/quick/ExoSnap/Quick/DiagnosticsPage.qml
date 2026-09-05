pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Diagnostics nav area. Verdict band, responsive readiness tiles, worst-first
// cards and one bundled tip chip. Expert mode is a Settings-only control; this
// page is ordered by what the recorder is doing, not by a display mode.
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
    // this page is either always visible or reached by expanding one of these.
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

        // ── Page header: identity and the support-bundle export ─────────────
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

            Item {
                Layout.fillWidth: true
            }

            // Chromed, not quiet: this one writes a file to disk.
            ExoButton {
                text: root.diagnostics.bundleBusy ? qsTr("Creating…") : qsTr("Create support bundle")
                leadingGlyph: ExoGlyph.Folder
                enabled: !root.diagnostics.bundleBusy
                Accessible.description: qsTr("Create a diagnostic package to share with support")
                onClicked: bundleDialog.open()
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

                            // The one action the verdict band offers -- and a
                            // NEUTRAL one, not the accent.
                            //
                            // The band is already carrying a colour, and that
                            // colour is the verdict. An accent-filled slab inside
                            // it put a second, unrelated hue in the same container
                            // and read as the loudest thing on a page whose point
                            // is the state it is reporting. It still needs chrome
                            // (left quiet it rendered as a text run beside the
                            // timestamp above it), so it keeps the bordered
                            // neutral treatment and gives up the fill.
                            ExoButton {
                                text: root.diagnostics.checking ? qsTr("Checking…") : qsTr("Run Check")
                                leadingGlyph: ExoGlyph.Run
                                enabled: !root.diagnostics.checking
                                Layout.alignment: Qt.AlignRight
                                onClicked: root.diagnostics.runCheck()
                            }
                        }
                    }
                }

                // ── Live pipeline summary ───────────────────────────────────────
                //
                // Present only while something is recording, and ABOVE the
                // readiness tiles while it is: readiness answers "may I start",
                // which stops being the question the moment a recording is
                // running. Five tiles, one per question the page could not
                // answer while idle — is the pipeline healthy, where is the
                // bottleneck, is frame pacing healthy, is the encoder healthy,
                // is audio synchronous, is storage healthy.
                //
                // Every value comes from diagnostics::BuildLiveTiles over the
                // engine's own snapshot. The page classifies nothing: a tile's
                // tone IS PipelineHealth plus the engine's bottleneck
                // attribution, so this surface can never call a pipeline the
                // engine reported as Good a warning.
                ColumnLayout {
                    spacing: ExoTheme.spacingSm
                    visible: root.diagnostics.liveTilesVisible
                    Layout.fillWidth: true

                    DiagnosticsSectionHeader {
                        title: qsTr("LIVE RECORDING")
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
                                detail: liveTile.modelData.detail
                                tone: liveTile.modelData.tone
                                Layout.fillWidth: true
                                Layout.fillHeight: true
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

                // ── Worst-first cards ───────────────────────────────────────────
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

                // ── Hardware capabilities ───────────────────────────────────────
                //
                // This used to be a whole navigation destination. Collapsed by
                // default so a healthy page stays short, and so the DXGI
                // enumeration + NVENC probe behind it only runs when asked for.
                ExoDisclosure {
                    id: hardwareCapabilitiesSection

                    // No subtitle: the panel opens on DeviceAdapter's own summary
                    // line, which says the same thing with the real adapter name
                    // in it. Two explanatory paragraphs stacked on top of each
                    // other read as one of them being unread.
                    title: qsTr("Hardware capabilities")
                    Layout.fillWidth: true

                    body: Component {
                        DeviceCapabilityPanel {
                            device: root.device
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
