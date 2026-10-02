pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    objectName: "recordSourcePicker"

    required property RecordViewModelAdapter recordViewModel

    // The destination that owns this picker. Required because the picker cannot
    // derive it: it parents itself into the window overlay, so nothing in its
    // own scene position says which page put it there. Also the safe-area
    // source: the dialog centres itself on this item's rect, so the shell title
    // band is excluded without hardcoding the shell's own chrome height here.
    required property Item hostPage

    // The pending choice is seeded from the view model when the picker opens
    // and is committed by the footer action, Enter or a double click. Browsing
    // is not committing: the capture keeps its current source until then.
    //
    // `pendingIdentity`/`pendingKind` are the durable half of the selection.
    // `pendingTargetIndex` is only the last index that identity was seen at --
    // a list insert or a rescan may move it, so commit re-resolves the row by
    // identity instead of trusting the index. The type is carried too, so the
    // Windows tab can never confirm a display the user picked earlier.
    property int pendingTargetIndex: -1
    property int pendingCaptureMode: 0
    property string pendingPresetKey: ""
    property string pendingIdentity: ""
    property string pendingKind: ""

    property int currentTab: 0
    property string windowQuery: ""

    readonly property var regionPresetRows: recordViewModel.regionPresetOptions

    readonly property var displayRows: recordViewModel.displayTargetOptions
    // Filtered through the adapter so the match policy (label, short title, app
    // name and window title) is shared with the control channel rather than
    // reimplemented per surface. The extra read of windowTargetOptions keeps
    // the binding alive across a rescan.
    readonly property var windowRows: {
        root.recordViewModel.windowTargetOptions;
        return root.recordViewModel.filteredTargetOptions("window", root.windowQuery);
    }
    readonly property int windowTotalCount: recordViewModel.windowTargetOptions.length

    readonly property string windowsCountText: windowRows.length === 1 ? qsTr("1 window")
                                                                       : qsTr("%1 windows").arg(windowRows.length)

    // ── Responsive geometry ──────────────────────────────────────────────────
    //
    // Columns follow the spec's formula: the smallest count whose cards can
    // stay at the 320 px target, capped at 4. Card width is then clamped at
    // 420 and the used grid width recomputed so the rows centre instead of the
    // cards stretching. All of this works in logical QML pixels.
    readonly property int gridGap: ExoTheme.spacingLg
    readonly property int contentPadding: ExoTheme.spacingXl
    readonly property real minCardWidth: 320
    readonly property real maxCardWidth: 420

    // The safe area is the host page, mapped into the window's content item --
    // not the popup's own `parent`, whose overlay can still carry the transient
    // size from before the shell's first resize. The shell title bar belongs to
    // the content item but not to the host page, so the dialog never centres
    // itself under the window buttons.
    readonly property Item hostWindowContent: root.hostPage.Window.window !== null
                                              && root.hostPage.Window.window !== undefined
                                              ? root.hostPage.Window.window.contentItem : null
    readonly property real safeWidth: Math.max(1, Math.min(root.hostWindowContent !== null
                                                           ? root.hostWindowContent.width : root.hostPage.width,
                                                           root.hostPage.width))
    readonly property real safeHeight: Math.max(1, Math.min(root.hostWindowContent !== null
                                                            ? root.hostWindowContent.height : root.hostPage.height,
                                                            root.hostPage.height))
    // 24 px of breathing room normally; 16 when the safe area is too narrow to
    // afford it at the product's minimum window.
    readonly property real outerInset: root.safeWidth < 760 ? ExoTheme.spacingLg : ExoTheme.spacingXl

    function columnsForWidth(availableWidth: real): int {
        const span = root.minCardWidth + root.gridGap;
        const columns = Math.floor((Math.max(0, availableWidth) + root.gridGap) / span);
        return Math.max(1, Math.min(4, columns));
    }

    // GridView has no inter-cell spacing and derives its column count from
    // floor(rowWidth / cellWidth), so the gap is folded into the pitch and the
    // card is sized so that `columns * pitch` never exceeds the content width.
    // The delegate is then one gap shorter than its cell, which is the single
    // place the gap is subtracted.
    function cardWidthFor(availableWidth: real, columns: int): real {
        const count = Math.max(1, columns);
        if (count === 1)
            return Math.max(1, Math.min(root.maxCardWidth, availableWidth));
        const perCard = Math.floor((Math.max(0, availableWidth) - count * root.gridGap) / count);
        return Math.max(1, Math.min(root.maxCardWidth, perCard));
    }

    // The card is a 16:9 media plate plus a fixed two-line title block and a
    // secondary line. Nothing about the source or its image can change a card's
    // height, so a row of cards always lines up.
    function cardHeightFor(cardWidth: real): real {
        const mediaHeight = Math.round(cardWidth * 9 / 16);
        const titleLine = Math.ceil(ExoTheme.fontBody * 1.3);
        const secondaryLine = Math.ceil(ExoTheme.fontCaption * 1.3);
        return mediaHeight + ExoTheme.spacingMd + 2 * titleLine + ExoTheme.spacingXs
                + secondaryLine + ExoTheme.spacingMd;
    }

    parent: Overlay.overlay
    padding: root.contentPadding
    width: Math.max(320, Math.min(1440, root.safeWidth - 2 * root.outerInset,
                                     Math.max(720, root.safeWidth * 0.8)))
    height: Math.max(240, Math.min(root.safeHeight - 2 * root.outerInset,
                                   root.chromeHeight + root.naturalGridHeight))

    // x/y are assigned imperatively rather than bound: a Popup positions itself
    // when it opens, and that internal write breaks a binding on them. The item
    // centred on is the window's content item -- not the popup's own `parent`,
    // which is a popup-private overlay whose size can still be the transient
    // one from before the shell's first resize.
    function recenter(): void {
        if (!root.visible || root.hostPage === null)
            return;
        const window = root.hostPage.Window.window;
        const host = (window !== null && window !== undefined) ? window.contentItem
                                                               : (root.parent !== null ? root.parent : root.hostPage);
        if (host === null || host === undefined)
            return;
        const origin = root.hostPage.mapToItem(host, 0, 0);
        const safe_w = Math.max(1, Math.min(host.width, root.hostPage.width));
        const safe_h = Math.max(1, Math.min(host.height, root.hostPage.height));
        root.x = Math.max(0, Math.min(origin.x + (safe_w - root.width) / 2, host.width - root.width));
        root.y = Math.max(0, Math.min(origin.y + (safe_h - root.height) / 2, host.height - root.height));
    }

    onWidthChanged: root.recenter()
    onHeightChanged: root.recenter()
    modal: true
    // The style's own modal veil lightens the shell in the dark palette, which
    // reads as the window coming forward rather than stepping back. Same scrim
    // token as the in-window overlay cards, so every modal recedes alike.
    Overlay.modal: Rectangle {
        color: ExoTheme.overlayScrim
    }
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property real gridContentWidth: Math.max(0, width - 2 * root.contentPadding)
    readonly property int pickerColumns: root.columnsForWidth(root.gridContentWidth)

    // Fewer displays than the width would allow must not leave an oversized
    // empty list: the display grid uses one column per display, and the cards
    // then cap at 420 instead of stretching. The window grid keeps the width
    // policy -- the search may not reflow the dialog on every keystroke.
    readonly property int displayColumns: Math.max(1, Math.min(root.pickerColumns, root.displayRows.length))
    readonly property int windowColumns: root.pickerColumns

    readonly property real displaysCardWidth: root.cardWidthFor(root.gridContentWidth, root.displayColumns)
    readonly property real windowsCardWidth: root.cardWidthFor(root.gridContentWidth, root.windowColumns)
    readonly property real displaysCardHeight: root.cardHeightFor(root.displaysCardWidth)
    readonly property real windowsCardHeight: root.cardHeightFor(root.windowsCardWidth)
    readonly property real displaysCellHeight: root.displaysCardHeight + root.gridGap
    readonly property real windowsCellHeight: root.windowsCardHeight + root.gridGap
    // One card plus one gap is a row pitch; a grid on its own is one card tall.

    // Rows the dialog reserves height for. For the Windows tab it is the
    // UNFILTERED count, so typing in the search field never makes the dialog
    // grow and shrink under the card the user is aiming at.
    readonly property int heightRowCount: root.currentTab === 1 ? root.windowTotalCount
                                          : root.currentTab === 0 ? root.displayRows.length : 0
    readonly property real naturalGridHeight: {
        if (root.currentTab === 2)
            return 190;
        const columns = root.currentTab === 1 ? root.windowColumns : root.displayColumns;
        const cellHeight = root.currentTab === 1 ? root.windowsCellHeight : root.displaysCellHeight;
        return Math.ceil(root.heightRowCount / Math.max(1, columns)) * cellHeight;
    }

    // ── Pending identity contract ────────────────────────────────────────────

    function targetRow(kind: string, identity: string): var {
        if (identity === "")
            return null;
        const rows = kind === "display" ? root.recordViewModel.displayTargetOptions
                                        : root.recordViewModel.windowTargetOptions;
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].identity === identity)
                return rows[i];
        }
        return null;
    }

    function resolvePendingRow(): var {
        return root.targetRow(root.pendingKind, root.pendingIdentity);
    }

    function targetPrimaryLabel(row: var): string {
        if (!row)
            return "";
        const title = row.title !== undefined ? row.title : "";
        return title !== "" ? title : row.label;
    }

    readonly property string expectedKind: root.currentTab === 0 ? "display"
                                          : root.currentTab === 1 ? "window" : ""
    readonly property var pendingRow: root.resolvePendingRow()
    readonly property bool pendingMatchesTab: root.pendingKind !== "" && root.pendingKind === root.expectedKind
    readonly property bool pendingMissing: root.pendingMatchesTab && root.pendingRow === null

    readonly property bool confirmEnabled: {
        if (root.currentTab === 2)
            return root.pendingPresetKey !== "";
        return root.pendingMatchesTab && root.pendingRow !== null;
    }

    readonly property bool footerWarning: root.currentTab !== 2 && root.pendingMissing

    function regionPresetRow(key: string): var {
        const rows = root.regionPresetRows;
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].key === key)
                return rows[i];
        }
        return null;
    }

    function footerStatusText(): string {
        if (root.currentTab === 2) {
            if (root.pendingPresetKey === "")
                return qsTr("Select a region preset");
            const preset = root.regionPresetRow(root.pendingPresetKey);
            return preset ? qsTr("Selected: %1").arg(preset.label) : qsTr("Select a region preset");
        }
        if (!root.pendingMatchesTab)
            return root.currentTab === 0 ? qsTr("Select a display to capture") : root.windowsCountText;
        if (root.pendingRow === null)
            return qsTr("Selected source is no longer available.");
        return qsTr("Selected: %1").arg(root.targetPrimaryLabel(root.pendingRow));
    }

    // ── Visible-target publication ───────────────────────────────────────────
    //
    // The identities the two grids currently have inside their viewport, in
    // layout order. The still service walks exactly this list, so a card that
    // is scrolled away stops costing a capture. Columns are derived from the
    // grid's own used width rather than the dialog's, because the display grid
    // may be centred narrower than the window grid.
    // The identities the two grids currently have inside their viewport, in
    // layout order. The still service walks exactly this list, so a card that
    // is scrolled away stops costing a capture. Columns are derived from the
    // grid's own used width rather than the dialog's, because the display grid
    // may be centred narrower than the window grid.
    function identitiesInView(grid: GridView, rows: var): var {
        if (!grid || !grid.visible || grid.cellHeight <= 0 || grid.cellWidth <= 0 || rows.length === 0)
            return []
        const columns = Math.max(1, Math.round(grid.width / grid.cellWidth))
        const firstRow = Math.max(0, Math.floor(grid.contentY / grid.cellHeight))
        const lastRow = Math.floor((grid.contentY + grid.height - 1) / grid.cellHeight)
        const identities = []
        for (let index = firstRow * columns; index <= (lastRow + 1) * columns - 1 && index < rows.length; ++index)
            identities.push(rows[index].identity)
        return identities
    }

    function publishVisibleTargets(): void {
        let identities = []
        if (root.visible && root.currentTab === 0)
            identities = root.identitiesInView(displaysGrid, root.displayRows)
        else if (root.visible && root.currentTab === 1)
            identities = root.identitiesInView(windowsGrid, root.windowRows)
        // The Region tab's cards are preset rectangles, not capture targets.
        root.recordViewModel.setVisibleTargetIdentities(identities)
    }

    onCurrentTabChanged: visiblePublishDelay.restart()
    onWindowRowsChanged: visiblePublishDelay.restart()

    // Hiding the Record destination does not take this popup down: it parents
    // itself into the window overlay (see `parent` above), so it has no view of
    // the destination it belongs to and `parent.visible` answers for the
    // overlay instead. The modal veil stops a click on another destination, but
    // not the keyboard shortcut that reaches the same swap -- which left the
    // picker on screen over the page behind it.
    Connections {
        target: root.hostPage
        enabled: root.opened

        function onVisibleChanged(): void {
            if (!root.hostPage.visible)
                root.close();
        }
    }

    Timer {
        id: visiblePublishDelay

        // Scrolling emits contentY continuously; republishing on every pixel
        // would reorder the round robin faster than a single grab completes.
        interval: 150
        onTriggered: root.publishVisibleTargets()
    }

    onAboutToShow: {
        root.windowQuery = "";
        windowSearchField.text = "";
        const mode = root.recordViewModel.captureMode;
        root.pendingCaptureMode = mode;
        root.pendingTargetIndex = root.recordViewModel.selectedTargetIndex;
        root.pendingPresetKey = "";
        root.pendingIdentity = "";
        root.pendingKind = "";
        if (root.pendingTargetIndex < 0)
            return;
        const rows = root.recordViewModel.targetOptions;
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].targetIndex === root.pendingTargetIndex) {
                root.pendingIdentity = rows[i].identity;
                root.pendingKind = rows[i].kind;
                break;
            }
        }
    }

    // Not onAboutToShow: the popup is not visible yet there, and the grids have
    // no height to derive a viewport from.
    onOpened: {
        root.recenter();
        visiblePublishDelay.restart();
    }

    // One bounded settle pass after open: the host's own first layout can land
    // after the popup is visible, and the dialog must not keep the transient
    // position. The popup closes before this fires only on a click-through.
    Timer {
        interval: 200
        running: root.opened
        onTriggered: root.recenter()
    }

    // Any later size change of either side re-centres it, at whatever size the
    // shell has settled on.
    Connections {
        target: root.hostPage !== null ? root.hostPage.Window.window : null
        enabled: root.opened

        function onWidthChanged(): void {
            root.recenter();
        }

        function onHeightChanged(): void {
            root.recenter();
        }
    }

    // The service is told the picker is gone rather than being left to poll a
    // hidden popup: a closed picker must not capture anything at all.
    onClosed: recordViewModel.setVisibleTargetIdentities([])

    function selectCard(row: var, captureMode: int): void {
        root.pendingTargetIndex = row.targetIndex;
        root.pendingCaptureMode = captureMode;
        root.pendingIdentity = row.identity;
        root.pendingKind = row.kind;
    }

    function commit(): void {
        if (root.currentTab === 2) {
            if (root.pendingPresetKey === "")
                return;
            const anchor = regionAnchorRow();
            if (!anchor)
                return;
            recordViewModel.requestSelectTarget(anchor.targetIndex, 2);
            recordViewModel.requestRegionPreset(root.pendingPresetKey);
            close();
            return;
        }
        // Re-resolve by identity at the last moment: a rescan between the click
        // and this call must not turn the stale index into a different source.
        if (!root.confirmEnabled)
            return;
        const row = root.resolvePendingRow();
        if (!row)
            return;
        recordViewModel.requestSelectTarget(row.targetIndex, root.pendingCaptureMode);
        close();
    }

    function confirmCard(row: var, captureMode: int): void {
        root.selectCard(row, captureMode);
        root.commit();
    }

    function commitPreset(key: string): void {
        root.pendingPresetKey = key;
        root.commit();
    }

    // The display a Region-tab rectangle anchors on: the display the user has
    // pending, else the one already capturing, else the first display.
    function regionAnchorRow(): var {
        const rows = recordViewModel.displayTargetOptions
        let fallback = null
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (root.pendingIdentity !== "" && row.identity === root.pendingIdentity)
                return row
            if (fallback === null && row.targetIndex === recordViewModel.selectedTargetIndex)
                fallback = row
        }
        return fallback !== null ? fallback : (rows.length > 0 ? rows[0] : null)
    }

    background: Rectangle {
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusLg
    }

    contentItem: ColumnLayout {
        id: pickerContent

        spacing: ExoTheme.spacingLg

        Rectangle {
            id: pickerHeader

            objectName: "pickerHeader"
            Layout.fillWidth: true
            implicitHeight: headerFlow.implicitHeight + 2 * ExoTheme.spacingMd
            color: ExoTheme.accentTint(ExoTheme.surfaceRaised, 0.05)
            radius: ExoTheme.radiusSm

            // A Flow rather than a Layout: when the safe width cannot hold the
            // title and the tabs side by side, the tabs drop to a second line
            // instead of squeezing the labels.
            Flow {
                id: headerFlow

                readonly property bool stacked: width > 0
                                                && titleBlock.width + tabsControl.width + 2 * ExoTheme.spacingMd
                                                   > width

                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: ExoTheme.spacingMd
                }
                spacing: ExoTheme.spacingMd

                Item {
                    id: titleBlock

                    width: Math.min(titleColumn.implicitWidth, headerFlow.width)
                    height: titleColumn.implicitHeight

                    Column {
                        id: titleColumn

                        width: titleBlock.width
                        spacing: ExoTheme.spacingXs

                        Label {
                            objectName: "pickerTitle"
                            width: parent.width
                            text: qsTr("Choose capture source")
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: ExoTheme.text
                            font.family: ExoTheme.sansFamily
                            font.pixelSize: ExoTheme.fontSectionTitle
                            font.weight: Font.DemiBold
                        }

                        Label {
                            width: parent.width
                            text: qsTr("Choose a display, window, or region.")
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: ExoTheme.textSecondary
                            font.family: ExoTheme.sansFamily
                            font.pixelSize: ExoTheme.fontSecondary
                        }
                    }
                }

                Item {
                    id: headerSpacer

                    width: headerFlow.stacked ? 0
                           : Math.max(0, headerFlow.width - titleBlock.width - tabsControl.width
                                         - 2 * ExoTheme.spacingMd)
                    height: 1
                }

                ExoSegmentedControl {
                    id: tabsControl

                    objectName: "tabs"
                    options: [qsTr("Displays"), qsTr("Windows"), qsTr("Region")]
                    currentIndex: root.currentTab
                    onSelected: index => root.currentTab = index
                }
            }
        }

        RowLayout {
            id: searchRow

            objectName: "windowSearchRow"
            Layout.fillWidth: true
            visible: root.currentTab === 1
            spacing: ExoTheme.spacingMd

            ExoSearchField {
                id: windowSearchField

                objectName: "windowSearch"
                Layout.fillWidth: true
                placeholderText: qsTr("Search windows...")
                onSearchEdited: query => root.windowQuery = query
            }
        }

        Item {
            id: gridArea

            objectName: "gridArea"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            Layout.preferredHeight: root.naturalGridHeight
            implicitHeight: root.naturalGridHeight
            clip: true

            Item {
                id: displaysPage

                objectName: "displaysPage"
                visible: root.currentTab === 0
                anchors.fill: parent

                GridView {
                    id: displaysGrid

                    objectName: "displaysGrid"
                    x: Math.round((parent.width - (width - root.gridGap)) / 2)
                    width: root.displayColumns * (root.displaysCardWidth + root.gridGap)
                    height: Math.floor(parent.height / cellHeight) * cellHeight
                    clip: true
                    model: root.displayRows
                    cellWidth: root.displaysCardWidth + root.gridGap
                    cellHeight: root.displaysCardHeight + root.gridGap
                    boundsBehavior: Flickable.StopAtBounds
                    onContentYChanged: visiblePublishDelay.restart()
                    onHeightChanged: visiblePublishDelay.restart()
                    onWidthChanged: visiblePublishDelay.restart()
                    onCellWidthChanged: visiblePublishDelay.restart()
                    ScrollBar.vertical: ExoScrollBar {
                    }

                    delegate: TargetCard {
                        captureMode: 0
                        width: GridView.view.cellWidth - root.gridGap
                        height: GridView.view.cellHeight - root.gridGap
                    }
                }

                Label {
                    objectName: "displaysEmptyState"
                    anchors.centerIn: parent
                    visible: root.displayRows.length === 0
                    text: qsTr("No displays found.")
                    textFormat: Text.PlainText
                    color: ExoTheme.textMuted
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontSecondary
                }
            }

            Item {
                id: windowsPage

                objectName: "windowsPage"
                visible: root.currentTab === 1
                anchors.fill: parent

                GridView {
                    id: windowsGrid

                    objectName: "windowsGrid"
                    x: Math.round((parent.width - (width - root.gridGap)) / 2)
                    width: root.windowColumns * (root.windowsCardWidth + root.gridGap)
                    height: Math.floor(parent.height / cellHeight) * cellHeight
                    clip: true
                    model: root.windowRows
                    cellWidth: root.windowsCardWidth + root.gridGap
                    cellHeight: root.windowsCardHeight + root.gridGap
                    boundsBehavior: Flickable.StopAtBounds
                    onContentYChanged: visiblePublishDelay.restart()
                    onHeightChanged: visiblePublishDelay.restart()
                    onWidthChanged: visiblePublishDelay.restart()
                    onCellWidthChanged: visiblePublishDelay.restart()
                    ScrollBar.vertical: ExoScrollBar {
                        objectName: "windowsScrollBar"
                    }

                    delegate: TargetCard {
                        captureMode: 1
                        width: GridView.view.cellWidth - root.gridGap
                        height: GridView.view.cellHeight - root.gridGap
                    }
                }

                Label {
                    objectName: "windowsEmptyState"
                    anchors.centerIn: parent
                    visible: root.windowRows.length === 0
                    text: root.windowTotalCount === 0 ? qsTr("No windows to capture.")
                                                      : qsTr("No windows match your search.")
                    textFormat: Text.PlainText
                    color: ExoTheme.textMuted
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontSecondary
                }
            }

            Item {
                id: regionPage

                objectName: "regionPage"
                visible: root.currentTab === 2
                anchors.fill: parent

                ColumnLayout {
                    anchors.fill: parent
                    spacing: ExoTheme.spacingMd

                    Label {
                        objectName: "regionCaption"

                        text: {
                            const anchor = root.regionAnchorRow()
                            return anchor ? qsTr("A preset starts an editable rectangle on %1.").arg(anchor.regionLabel)
                                          : qsTr("Select a display first.")
                        }
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: ExoTheme.textSecondary
                        Layout.fillWidth: true
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: ExoTheme.fontSecondary
                    }

                    Flow {
                        spacing: ExoTheme.spacingSm
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        Repeater {
                            model: root.regionPresetRows

                            delegate: PresetCard {
                                width: Math.min(190, (regionPage.width - 2 * ExoTheme.spacingSm) / 3)
                                height: 128
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            id: footerRow

            objectName: "pickerFooter"
            Layout.fillWidth: true
            spacing: ExoTheme.spacingSm

            Label {
                objectName: "windowsCount"
                Layout.fillWidth: true
                text: root.footerStatusText()
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: root.footerWarning ? ExoTheme.warningText : ExoTheme.textSecondary
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontSecondary
            }

            ExoButton {
                objectName: "cancelButton"

                text: qsTr("Cancel")
                quiet: true
                onClicked: root.close()
            }

            ExoButton {
                objectName: "confirmButton"

                text: qsTr("Use source")
                tone: "primary"
                enabled: root.confirmEnabled
                onClicked: root.commit()
            }
        }
    }

    readonly property real chromeHeight: root.contentPadding * 2
                                         + pickerHeader.implicitHeight
                                         + (searchRow.visible ? searchRow.implicitHeight : 0)
                                         + footerRow.implicitHeight
                                         + ExoTheme.spacingLg * (searchRow.visible ? 3 : 2)

    component TargetCard: Rectangle {
        id: card

        // A QVariantList model exposes one role, modelData, whose map keys do
        // not initialize per-key required properties -- so the row arrives as
        // modelData and the typed surface is derived from it.
        required property var modelData

        readonly property int targetIndex: card.modelData.targetIndex
        readonly property string identity: card.modelData.identity
        readonly property string label: card.modelData.label
        readonly property string kind: card.modelData.kind
        readonly property string appName: card.modelData.appName
        readonly property string windowTitle: card.modelData.windowTitle
        readonly property string primaryLabel: {
            const title = card.modelData.title !== undefined ? card.modelData.title : "";
            return title !== "" ? title : card.label;
        }
        // The window's app is the secondary identity; a display has no measured
        // resolution or refresh data in the engine's target row, so it shows no
        // invented value instead.
        readonly property string secondaryLabel: card.kind === "window" && card.windowTitle !== "" ? card.appName : ""
        readonly property var still: root.recordViewModel.targetStillOptions[card.identity]
        readonly property string thumbnailState: card.still ? card.still.state : "placeholder"
        readonly property string thumbnailSource: card.still ? card.still.source : ""

        property int captureMode: 0
        readonly property bool pending: root.pendingIdentity !== "" && card.identity === root.pendingIdentity
                                        && root.pendingKind === card.kind
                                        && root.pendingCaptureMode === card.captureMode

        objectName: "targetCard-" + card.identity
        color: card.pending ? ExoTheme.accentTint(ExoTheme.surfaceRaised, 0.14)
                            : cardHover.hovered ? ExoTheme.surfaceHover : ExoTheme.surface
        border.width: card.pending ? 2 : 1
        border.color: card.pending ? ExoTheme.accent
                      : card.activeFocus ? ExoTheme.text
                      : cardHover.hovered ? ExoTheme.lineStrong : ExoTheme.line
        radius: ExoTheme.radiusSm
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: card.label

        Keys.onReturnPressed: event => {
            root.confirmCard(card.modelData, card.captureMode)
            event.accepted = true
        }
        Keys.onEnterPressed: event => {
            root.confirmCard(card.modelData, card.captureMode)
            event.accepted = true
        }
        Keys.onSpacePressed: event => {
            root.selectCard(card.modelData, card.captureMode)
            event.accepted = true
        }

        HoverHandler {
            id: cardHover
            cursorShape: Qt.PointingHandCursor
        }

        ToolTip {
            text: card.label
            visible: (cardHover.hovered || card.activeFocus) && card.label !== ""
            delay: 400
        }

        MouseArea {
            anchors.fill: parent
            onClicked: root.selectCard(card.modelData, card.captureMode)
            onDoubleClicked: root.confirmCard(card.modelData, card.captureMode)
        }

        Rectangle {
            id: thumbnail

            objectName: "targetThumbnail"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: Math.round(card.width * 9 / 16)
            color: ExoTheme.overlaySurface
            border.width: 0
            radius: ExoTheme.radiusSm

            Image {
                objectName: "targetThumbnailImage"
                anchors.fill: parent
                anchors.margins: 2
                source: card.thumbnailSource
                visible: card.thumbnailState !== "placeholder"
                // A target that stopped being capturable keeps its last still,
                // dimmed. Clearing it would resize nothing but would make every
                // minimized window flicker back to a glyph and out again.
                opacity: card.thumbnailState === "stale" ? 0.45 : 1.0
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                retainWhileLoading: true
                sourceSize: Qt.size(width, height)
            }

            ExoGlyph {
                anchors.centerIn: parent
                kind: card.kind === "window" ? ExoGlyph.AppWindow : ExoGlyph.Display
                visible: card.thumbnailState === "placeholder"
                color: ExoTheme.overlayInkDim
                width: 22
                height: 22
            }

            Label {
                objectName: "thumbnailStateLabel"

                anchors {
                    left: parent.left
                    bottom: parent.bottom
                    margins: ExoTheme.spacingSm
                }
                visible: card.thumbnailState === "stale"
                text: qsTr("Preview not updating")
                textFormat: Text.PlainText
                color: ExoTheme.overlayInkSecondary
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontCaption
            }

            ExoGlyph {
                anchors {
                    top: parent.top
                    right: parent.right
                    margins: ExoTheme.spacingXs
                }
                kind: ExoGlyph.Check
                visible: card.pending
                color: ExoTheme.accent
                width: 16
                height: 16
            }
        }

        Item {
            anchors {
                left: parent.left
                right: parent.right
                top: thumbnail.bottom
                bottom: parent.bottom
                leftMargin: ExoTheme.spacingMd
                rightMargin: ExoTheme.spacingMd
                topMargin: ExoTheme.spacingSm
                bottomMargin: ExoTheme.spacingSm
            }

            Column {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                spacing: ExoTheme.spacingXs

                Label {
                    width: parent.width
                    text: card.primaryLabel
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    color: ExoTheme.text
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontBody
                    font.weight: Font.DemiBold
                }

                Label {
                    width: parent.width
                    visible: card.secondaryLabel !== ""
                    text: card.secondaryLabel
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: ExoTheme.textSecondary
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontCaption
                }
            }
        }
    }

    component PresetCard: Rectangle {
        id: presetCard

        // Same modelData shape as TargetCard: the row is a QVariantMap entry.
        required property var modelData

        readonly property string key: presetCard.modelData.key
        readonly property string label: presetCard.modelData.label
        readonly property real aspect: presetCard.modelData.aspect
        readonly property bool draw: presetCard.modelData.draw

        readonly property bool pendingPreset: root.pendingPresetKey === presetCard.key
        readonly property bool emphasized: presetCard.draw

        objectName: "presetCard-" + presetCard.key
        color: presetCard.pendingPreset ? ExoTheme.surfaceHover : ExoTheme.surface
        border.width: presetCard.pendingPreset ? 2 : 1
        border.color: presetCard.pendingPreset ? ExoTheme.accent
                     : presetCard.emphasized ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: presetCard.label

        Keys.onReturnPressed: event => {
            root.commitPreset(presetCard.key)
            event.accepted = true
        }
        Keys.onEnterPressed: event => {
            root.commitPreset(presetCard.key)
            event.accepted = true
        }
        Keys.onSpacePressed: event => {
            root.pendingPresetKey = presetCard.key
            event.accepted = true
        }

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }

        MouseArea {
            anchors.fill: parent
            onClicked: root.pendingPresetKey = presetCard.key
            onDoubleClicked: root.commitPreset(presetCard.key)
        }

        ExoGlyph {
            id: drawGlyph

            anchors.centerIn: parent
            kind: ExoGlyph.Region
            visible: presetCard.draw
            color: presetCard.pendingPreset ? ExoTheme.accent : ExoTheme.textSecondary
            width: 30
            height: 30
        }

        Rectangle {
            id: previewShape

            visible: !presetCard.draw
            readonly property real maxW: 88
            readonly property real maxH: 56
            readonly property real fitW: presetCard.aspect >= 1.0 ? maxW : Math.min(maxW, maxH * presetCard.aspect)
            readonly property real fitH: presetCard.aspect >= 1.0 ? Math.min(maxH, maxW / presetCard.aspect) : maxH

            width: Math.max(10, fitW)
            height: Math.max(10, fitH)
            radius: 2
            color: "#00000000"
            border.width: 2
            border.color: presetCard.pendingPreset ? ExoTheme.accent : ExoTheme.textDim
            anchors.centerIn: parent
            anchors.verticalCenterOffset: -10
        }

        Label {
            text: presetCard.label
            textFormat: Text.PlainText
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            color: presetCard.pendingPreset || presetCard.emphasized ? ExoTheme.text : ExoTheme.textSecondary
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontSecondary
            font.weight: presetCard.emphasized ? Font.DemiBold : Font.Medium
            anchors {
                bottom: parent.bottom
                left: parent.left
                right: parent.right
                margins: ExoTheme.spacingSm
            }
        }
    }
}
