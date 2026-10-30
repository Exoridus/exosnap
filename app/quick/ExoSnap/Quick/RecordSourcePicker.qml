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
    // `pendingTargetIndex` is only the last index that identity was seen at.
    // Confirmation passes the identity to C++ for a fresh lifetime check.
    property int pendingTargetIndex: -1
    property int pendingCaptureMode: 0
    property string pendingPresetKey: ""
    property string pendingIdentity: ""
    property string pendingKind: ""

    property int currentTab: 0
    property string windowQuery: ""
    property var hideCandidate: null

    readonly property var regionPresetRows: recordViewModel.regionPresetOptions

    readonly property var displayRows: recordViewModel.displayTargetOptions
    property var sessionWindowOrder: []
    readonly property var windowRows: {
        const rows = root.recordViewModel.filteredWindowTargetOptions.slice();
        return rows.sort((a, b) => {
            const first = root.sessionWindowOrder.indexOf(a.identity);
            const second = root.sessionWindowOrder.indexOf(b.identity);
            return (first < 0 ? root.sessionWindowOrder.length : first) - (second < 0 ? root.sessionWindowOrder.length : second);
        });
    }
    function updateWindowOrder(): void {
        const order = root.sessionWindowOrder.slice();
        for (const row of root.recordViewModel.windowTargetOptions)
            if (!order.includes(row.identity))
                order.push(row.identity);
        root.sessionWindowOrder = order;
    }
    Connections {
        target: root.recordViewModel
        function onTargetOptionsChanged(): void {
            root.updateWindowOrder();
        }
    }
    property var tabStates: [null, null, null]
    readonly property int windowTotalCount: recordViewModel.windowTargetOptions.length
    readonly property int hiddenWindowCount: recordViewModel.hiddenWindowCount
    readonly property int windowSearchMatchCount: recordViewModel.pickerSearchMatchCount
    onWindowQueryChanged: root.recordViewModel.setWindowSearchQuery(root.windowQuery)
    readonly property var hiddenWindowRules: recordViewModel.hiddenWindowRules

    property var pickerRows: []
    property int viewportRevision: 0
    // Queued refreshes retain the geometry and lifetime focus from before the first update.
    property var pendingViewport: null
    readonly property var incomingPickerRows: {
        const rows = [];
        function appendCards(kind, targets, columns) {
            rows.push({
                kind: "heading",
                section: kind
            });
            for (let index = 0; index < targets.length; index += columns)
                rows.push({
                    kind: "cards",
                    section: kind,
                    cards: targets.slice(index, index + columns)
                });
            if (targets.length === 0)
                rows.push({
                    kind: "empty",
                    section: kind
                });
        }
        if (root.currentTab === 0)
            appendCards("display", root.displayRows, root.displayColumns);
        else if (root.currentTab === 1)
            appendCards("window", root.windowRows, root.windowColumns);
        else {
            rows.push({
                kind: "heading",
                section: "region"
            });
            rows.push({
                kind: "region",
                section: "region"
            });
        }
        return rows;
    }

    readonly property string windowsCountText: qsTr("Windows: %n", "", windowRows.length)

    // ── Responsive geometry ──────────────────────────────────────────────────
    //
    // Logical card sizes stay independent of source resolution and search count.
    readonly property int gridGap: ExoTheme.spacingLg
    readonly property int contentPadding: ExoTheme.spacingXl
    readonly property real minCardWidth: 230
    readonly property real maxCardWidth: 260

    // The safe area is the host page, mapped into the window's content item --
    // not the popup's own `parent`, whose overlay can still carry the transient
    // size from before the shell's first resize. The shell title bar belongs to
    // the content item but not to the host page, so the dialog never centres
    // itself under the window buttons.
    readonly property Item hostWindowContent: root.hostPage.Window.window !== null && root.hostPage.Window.window !== undefined ? root.hostPage.Window.window.contentItem : null
    readonly property real safeWidth: Math.max(1, Math.min(root.hostWindowContent !== null ? root.hostWindowContent.width : root.hostPage.width, root.hostPage.width))
    readonly property real safeHeight: Math.max(1, Math.min(root.hostWindowContent !== null ? root.hostWindowContent.height : root.hostPage.height, root.hostPage.height))
    // 24 px of breathing room normally; 16 when the safe area is too narrow to
    // afford it at the product's minimum window.
    readonly property real outerInset: root.safeWidth < 760 ? ExoTheme.spacingLg : ExoTheme.spacingXl

    function columnsForWidth(availableWidth: real): int {
        const span = root.minCardWidth + root.gridGap;
        const columns = Math.floor((Math.max(0, availableWidth) + root.gridGap) / span);
        return Math.max(1, Math.min(4, columns));
    }

    // Keep the existing card widths while grouping no more than four cards in
    // each virtualized list row. The cap limits layout work, not window count.
    function cardWidthFor(availableWidth: real, columns: int): real {
        const count = Math.max(1, columns);
        if (count === 1)
            return Math.max(1, Math.min(root.maxCardWidth, availableWidth));
        const perCard = Math.floor((Math.max(0, availableWidth) - (count - 1) * root.gridGap) / count);
        return Math.max(1, Math.min(root.maxCardWidth, perCard));
    }

    // The card is a 16:9 media plate plus a fixed title block and a
    // secondary line. Nothing about the source or its image can change a card's
    // height, so a row of cards always lines up.
    function cardHeightFor(cardWidth: real): real {
        const mediaHeight = Math.round(cardWidth * 9 / 16);
        const titleLine = Math.ceil(ExoTheme.fontBody * 1.3);
        const secondaryLine = Math.ceil(ExoTheme.fontCaption * 1.3);
        return mediaHeight + ExoTheme.spacingMd + titleLine + ExoTheme.spacingXs + secondaryLine + ExoTheme.spacingMd;
    }

    parent: Overlay.overlay
    padding: root.contentPadding
    width: Math.max(320, Math.min(1440, root.safeWidth - 2 * root.outerInset, Math.max(720, root.safeWidth * 0.8)))
    height: Math.max(240, root.safeHeight - 2 * root.outerInset)

    // x/y are assigned imperatively rather than bound: a Popup positions itself
    // when it opens, and that internal write breaks a binding on them. The item
    // centred on is the window's content item -- not the popup's own `parent`,
    // which is a popup-private overlay whose size can still be the transient
    // one from before the shell's first resize.
    function recenter(): void {
        if (!root.visible || root.hostPage === null)
            return;
        const window = root.hostPage.Window.window;
        const host = (window !== null && window !== undefined) ? window.contentItem : (root.parent !== null ? root.parent : root.hostPage);
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

    // Fewer displays than the width would allow use fewer columns. Windows
    // retain the full width policy as the search result changes.
    readonly property int displayColumns: Math.max(1, Math.min(root.pickerColumns, root.displayRows.length))
    readonly property int windowColumns: root.pickerColumns

    readonly property real displaysCardWidth: root.cardWidthFor(root.gridContentWidth, root.displayColumns)
    readonly property real windowsCardWidth: root.cardWidthFor(root.gridContentWidth, root.windowColumns)
    readonly property real displaysCardHeight: root.cardHeightFor(root.displaysCardWidth)
    readonly property real windowsCardHeight: root.cardHeightFor(root.windowsCardWidth)
    readonly property real displaysCellHeight: root.displaysCardHeight + root.gridGap
    readonly property real windowsCellHeight: root.windowsCardHeight + root.gridGap
    // One card plus one gap is a row pitch; a grid on its own is one card tall.

    // ── Pending identity contract ────────────────────────────────────────────

    function targetRow(kind: string, identity: string): var {
        if (identity === "")
            return null;
        const rows = kind === "display" ? root.recordViewModel.displayTargetOptions : root.recordViewModel.windowTargetOptions;
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

    readonly property var pendingRow: root.resolvePendingRow()
    readonly property bool pendingMissing: root.pendingKind !== "" && root.pendingKind !== "region" && (root.pendingRow === null || root.pendingRow.available === false)

    readonly property bool confirmEnabled: {
        if (root.pendingKind === "region")
            return root.pendingPresetKey !== "";
        return root.pendingKind !== "" && root.pendingRow !== null && root.pendingRow.available !== false;
    }

    readonly property bool footerWarning: root.pendingMissing

    function regionPresetRow(key: string): var {
        const rows = root.regionPresetRows;
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].key === key)
                return rows[i];
        }
        return null;
    }

    function footerStatusText(): string {
        if (root.pendingKind === "region") {
            if (root.pendingPresetKey === "")
                return qsTr("Select a region preset");
            const preset = root.regionPresetRow(root.pendingPresetKey);
            return preset ? qsTr("Selected: %1").arg(preset.label) : qsTr("Select a region preset");
        }
        if (root.pendingKind === "")
            return qsTr("Select a display, window, or region.");
        if (root.pendingMissing)
            return qsTr("Selected source is no longer available.");
        return qsTr("Selected: %1").arg(root.targetPrimaryLabel(root.pendingRow));
    }

    // ── Visible-target publication ───────────────────────────────────────────
    //
    function publishVisibleTargets(): void {
        const identities = [];
        if (root.visible) {
            const top = sourceList.contentY - sourceList.originY;
            const bottom = top + sourceList.height;
            let offset = 0;
            for (const row of root.pickerRows) {
                const height = root.pickerRowHeight(row);
                if (offset < bottom && offset + height > top && row.kind === "cards") {
                    for (const card of row.cards)
                        identities.push(card.identity);
                }
                offset += height;
                if (offset >= bottom)
                    break;
            }
        }
        root.recordViewModel.setVisibleTargetIdentities(identities);
    }

    function pickerRowHeight(row: var): real {
        if (row.kind === "heading")
            return 38;
        if (row.kind === "region")
            return 196;
        if (row.kind === "empty")
            return 50;
        return row.section === "display" ? root.displaysCellHeight : root.windowsCellHeight;
    }

    function visibleThumbnailsReady(): bool {
        const top = sourceList.contentY - sourceList.originY;
        let y = 0;
        for (const row of root.pickerRows) {
            const height = root.pickerRowHeight(row);
            if (row.kind === "cards" && y < top + sourceList.height && y + height > top) {
                for (const target of row.cards) {
                    const card = root.findPickerItem(sourceList.contentItem, "targetCard-" + target.identity);
                    const image = (card ? root.findPickerItem(card, "targetThumbnailImage") : null) as Image;
                    if (!image || image.status !== Image.Ready || image.paintedWidth <= 0 || image.paintedHeight <= 0)
                        return false;
                }
            }
            y += height;
        }
        return true;
    }

    function selectTab(index: int): void {
        if (index === root.currentTab)
            return;
        const first = Math.max(0, sourceList.indexAt(1, sourceList.contentY + 1));
        const item = sourceList.itemAtIndex(first);
        const keys = [];
        for (let i = first; i < root.pickerRows.length; ++i)
            keys.push(...root.rowKeys(root.pickerRows[i]));
        for (let i = first - 1; i >= 0; --i)
            keys.push(...root.rowKeys(root.pickerRows[i]));
        root.tabStates[root.currentTab] = {
            keys: keys,
            offset: item ? item.y - sourceList.contentY : 0,
            identity: root.pendingIdentity,
            kind: root.pendingKind,
            targetIndex: root.pendingTargetIndex,
            mode: root.pendingCaptureMode,
            preset: root.pendingPresetKey
        };
        ++root.viewportRevision;
        root.pendingViewport = null;
        root.currentTab = index;
        ++root.viewportRevision;
        root.pendingViewport = null;
        const state = root.tabStates[index];
        root.pendingIdentity = state ? state.identity : "";
        root.pendingKind = state ? state.kind : "";
        root.pendingTargetIndex = state ? state.targetIndex : -1;
        root.pendingCaptureMode = state ? state.mode : index;
        root.pendingPresetKey = state ? state.preset : "";
        sourceList.forceLayout();
        const positions = {};
        for (let i = 0; i < root.pickerRows.length; ++i)
            for (const key of root.rowKeys(root.pickerRows[i]))
                positions[key] = i;
        const anchor = state ? state.keys.find(key => positions[key] !== undefined) : undefined;
        if (anchor !== undefined) {
            sourceList.positionViewAtIndex(positions[anchor], ListView.Beginning);
            sourceList.forceLayout();
            const row = sourceList.itemAtIndex(positions[anchor]);
            const end = sourceList.originY + Math.max(0, sourceList.contentHeight - sourceList.height);
            if (row)
                sourceList.contentY = Math.max(sourceList.originY, Math.min(row.y - state.offset, end));
        } else {
            sourceList.positionViewAtBeginning();
        }
        visiblePublishDelay.restart();
    }

    function rowKeys(row: var): var {
        return row.kind === "cards" ? row.cards.map(card => card.identity).filter(identity => identity !== "") : [row.kind + ":" + row.section];
    }

    ListModel {
        id: visibleRows
    }

    function setPickerRows(rows: var): void {
        root.pickerRows = rows;
        while (visibleRows.count > rows.length)
            visibleRows.remove(visibleRows.count - 1);
        while (visibleRows.count < rows.length)
            visibleRows.append({
                slot: visibleRows.count
            });
    }

    function findPickerItem(item: Item, name: string): Item {
        if (item.objectName === name)
            return item;
        for (const child of item.children) {
            const found = root.findPickerItem(child, name);
            if (found)
                return found;
        }
        return null;
    }

    function refreshPickerRows(): void {
        const next = root.incomingPickerRows;
        const previous = root.pickerRows;
        const sameLayout = previous.length === next.length && previous.every((row, index) => JSON.stringify(root.rowKeys(row)) === JSON.stringify(root.rowKeys(next[index])));
        const appended = previous.length <= next.length && previous.every((row, index) => {
            const keys = root.rowKeys(row);
            const nextKeys = root.rowKeys(next[index]);
            return keys.every((key, i) => key === nextKeys[i]);
        });
        if (!root.opened || sameLayout || appended || sourceList.moving || sourceScrollBar.pressed) {
            root.setPickerRows(next);
            return;
        }
        const revision = ++root.viewportRevision;
        const window = root.hostPage.Window.window;
        if (root.pendingViewport && window.activeFocusItem !== root.pendingViewport.focusOwner)
            root.pendingViewport = null;
        let viewport = root.pendingViewport;
        if (!viewport) {
            const focusedItem = window ? window.activeFocusItem : null;
            const focusedName = focusedItem ? focusedItem.objectName : "";
            const first = Math.max(0, sourceList.indexAt(1, sourceList.contentY + 1));
            const firstItem = sourceList.itemAtIndex(first);
            let offset = firstItem ? firstItem.y - sourceList.contentY : 0;
            const keys = [];
            if (focusedName.startsWith("targetCard-")) {
                const focusedY = focusedItem.mapToItem(sourceList, 0, 0).y;
                if (focusedY < sourceList.height && focusedY + focusedItem.height > 0) {
                    keys.push(focusedName.slice("targetCard-".length));
                    offset = focusedY;
                }
            }
            for (let i = first; i < previous.length; ++i)
                keys.push(...root.rowKeys(previous[i]));
            for (let i = first - 1; i >= 0; --i)
                keys.push(...root.rowKeys(previous[i]));
            if (focusedName.startsWith("targetCard-") || focusedName.startsWith("presetCard-"))
                root.contentItem.forceActiveFocus(Qt.TabFocusReason);
            viewport = {
                keys,
                offset,
                focusedName,
                focusOwner: window.activeFocusItem
            };
            root.pendingViewport = viewport;
        }
        const positions = {};
        for (let i = 0; i < next.length; ++i)
            for (const key of root.rowKeys(next[i]))
                positions[key] = i;
        const anchor = viewport.keys.find(key => positions[key] !== undefined);
        root.setPickerRows(next);
        Qt.callLater(() => {
            if (!root.opened || root.viewportRevision !== revision || sourceList.moving || sourceScrollBar.pressed)
                return;
            root.pendingViewport = null;
            if (window.activeFocusItem !== viewport.focusOwner)
                return;
            sourceList.forceLayout();
            if (anchor !== undefined) {
                sourceList.positionViewAtIndex(positions[anchor], ListView.Beginning);
                sourceList.forceLayout();
                const row = sourceList.itemAtIndex(positions[anchor]);
                if (row) {
                    const end = sourceList.originY + Math.max(0, sourceList.contentHeight - sourceList.height);
                    sourceList.contentY = Math.max(sourceList.originY, Math.min(row.y - viewport.offset, end));
                }
            }
            sourceList.forceLayout();
            if (viewport.focusedName.startsWith("targetCard-") || viewport.focusedName.startsWith("presetCard-")) {
                const focused = root.findPickerItem(sourceList.contentItem, viewport.focusedName);
                if (focused && !focused.activeFocus)
                    focused.forceActiveFocus(Qt.TabFocusReason);
            }
            visiblePublishDelay.restart();
        });
    }

    onIncomingPickerRowsChanged: root.refreshPickerRows()
    onPickerRowsChanged: visiblePublishDelay.restart()

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
        ++root.viewportRevision;
        root.pendingViewport = null;
        root.sessionWindowOrder = [];
        root.updateWindowOrder();
        root.tabStates = [null, null, null];
        root.currentTab = root.recordViewModel.captureMode;
        sourceList.positionViewAtBeginning();
        root.windowQuery = "";
        root.recordViewModel.setWindowSearchQuery("");
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
                if (mode !== 2)
                    root.currentTab = root.pendingKind === "window" ? 1 : 0;
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
    onClosed: {
        root.pendingViewport = null;
        recordViewModel.setVisibleTargetIdentities([]);
    }

    function selectCard(row: var, captureMode: int): void {
        if (row.available === false || row.identity === "")
            return;
        root.pendingTargetIndex = row.targetIndex;
        root.pendingCaptureMode = captureMode;
        root.pendingIdentity = row.identity;
        root.pendingKind = row.kind;
        root.pendingPresetKey = "";
    }

    function commit(): void {
        if (root.pendingKind === "region") {
            if (root.pendingPresetKey === "")
                return;
            const anchor = regionAnchorRow();
            if (!anchor)
                return;
            if (!recordViewModel.confirmTarget(anchor.identity, 2))
                return;
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
        if (recordViewModel.confirmTarget(row.identity, root.pendingCaptureMode))
            close();
    }

    function confirmCard(row: var, captureMode: int): void {
        if (row.available === false || row.identity === "")
            return;
        root.selectCard(row, captureMode);
        root.commit();
    }

    function commitPreset(key: string): void {
        root.pendingPresetKey = key;
        root.pendingKind = "region";
        root.commit();
    }

    function hideCandidateSession(): void {
        if (root.hideCandidate)
            recordViewModel.hideWindowInstance(root.hideCandidate.identity);
    }

    // The display a Region-tab rectangle anchors on: the display the user has
    // pending, else the one already capturing, else the first display.
    function regionAnchorRow(): var {
        const rows = recordViewModel.displayTargetOptions;
        const displayState = root.tabStates[0];
        const identity = root.currentTab === 2 && displayState ? displayState.identity : root.pendingIdentity;
        let fallback = null;
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i];
            if (identity !== "" && row.identity === identity)
                return row;
            if (fallback === null && row.targetIndex === recordViewModel.selectedTargetIndex)
                fallback = row;
        }
        return fallback !== null ? fallback : (rows.length > 0 ? rows[0] : null);
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

        RowLayout {
            id: pickerHeader
            objectName: "pickerHeader"
            Layout.fillWidth: true
            Label {
                objectName: "pickerTitle"
                Layout.fillWidth: true
                text: qsTr("Choose capture source")
                color: ExoTheme.text
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontSectionTitle
                font.weight: Font.DemiBold
            }
            ExoButton {
                objectName: "pickerClose"
                glyph: ExoGlyph.Close
                text: qsTr("Close")
                compact: true
                quiet: true
                onClicked: root.close()
            }
        }
        ExoSegmentedControl {
            id: tabsControl
            objectName: "tabs"
            options: [qsTr("Displays"), qsTr("Windows"), qsTr("Region")]
            Accessible.name: qsTr("Source type")
            currentIndex: root.currentTab
            onSelected: index => root.selectTab(index)
        }
        RowLayout {
            id: searchRow

            objectName: "windowSearchRow"
            visible: root.currentTab === 1
            Layout.fillWidth: true
            spacing: ExoTheme.spacingMd

            ExoSearchField {
                id: windowSearchField

                objectName: "windowSearch"
                Layout.fillWidth: true
                placeholderText: qsTr("Search windows...")
                onSearchEdited: query => root.windowQuery = query
            }

            Label {
                objectName: "searchMatchCount"
                text: qsTr("%1 search matches").arg(root.windowSearchMatchCount)
                color: ExoTheme.textSecondary
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontCaption
            }

            ExoButton {
                id: windowOptions
                property bool expanded: false
                text: qsTr("Options")
                quiet: true
                onClicked: expanded = !expanded
            }
        }
        RowLayout {
            visible: root.currentTab === 1 && windowOptions.expanded
            Layout.fillWidth: true
            ExoCheckBox {
                objectName: "showHiddenWindows"
                text: qsTr("Show hidden (%1)").arg(root.hiddenWindowCount)
                checked: root.recordViewModel.showHiddenWindows
                onToggledByUser: value => root.recordViewModel.setShowHiddenWindows(value)
            }

            ExoButton {
                objectName: "hiddenRulesButton"
                text: qsTr("Hidden rules")
                quiet: true
                onClicked: rulesPopup.open()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.currentTab === 1 && windowOptions.expanded
            text: qsTr("Hiding a window here does not hide it from a display recording.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.textSecondary
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontCaption
        }

        Item {
            id: gridArea
            objectName: "gridArea"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0

            ListView {
                id: sourceList
                objectName: "sourceList"
                anchors.fill: parent
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // Replacing an array model resets the viewport and cancels an active flick.
                model: visibleRows
                onContentYChanged: visiblePublishDelay.restart()
                onHeightChanged: visiblePublishDelay.restart()
                onWidthChanged: visiblePublishDelay.restart()
                onMovementStarted: {
                    ++root.viewportRevision;
                    root.pendingViewport = null;
                }
                ScrollBar.vertical: ExoScrollBar {
                    id: sourceScrollBar
                    objectName: "sourceScrollBar"
                    onPressedChanged: if (pressed) {
                        ++root.viewportRevision;
                        root.pendingViewport = null;
                    }
                }

                delegate: Item {
                    id: rowItem
                    required property int index
                    readonly property var modelData: root.pickerRows[rowItem.index] ?? {
                        kind: "empty",
                        section: "window"
                    }
                    width: sourceList.width
                    height: root.pickerRowHeight(rowItem.modelData)

                    Label {
                        objectName: rowItem.modelData.kind === "heading" ? rowItem.modelData.section + "SectionLabel" : ""
                        visible: rowItem.modelData.kind === "heading"
                        text: rowItem.modelData.section === "display" ? qsTr("Displays") : rowItem.modelData.section === "window" ? qsTr("Windows / Applications") : qsTr("Region")
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        color: ExoTheme.text
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: ExoTheme.fontSectionTitle
                        font.weight: Font.DemiBold
                    }

                    Label {
                        objectName: rowItem.modelData.kind !== "empty" ? "" : rowItem.modelData.section === "display" ? "displaysEmptyState" : "windowsEmptyState"
                        visible: rowItem.modelData.kind === "empty"
                        text: rowItem.modelData.section === "display" ? qsTr("No displays found.") : root.windowTotalCount === 0 ? qsTr("No windows to capture.") : root.windowSearchMatchCount === 0 ? qsTr("No windows match your search.") : qsTr("No visible windows. Show hidden windows to review them.")
                        anchors.centerIn: parent
                        color: ExoTheme.textMuted
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: ExoTheme.fontSecondary
                    }

                    Row {
                        x: Math.round((parent.width - implicitWidth) / 2)
                        spacing: root.gridGap
                        visible: rowItem.modelData.kind === "cards"
                        Repeater {
                            model: rowItem.modelData.kind === "cards" ? rowItem.modelData.cards.length : 0
                            delegate: TargetCard {
                                required property int index
                                modelData: rowItem.modelData.cards?.[index] ?? ({
                                        identity: "",
                                        targetIndex: -1,
                                        label: "",
                                        kind: "",
                                        appName: "",
                                        windowTitle: "",
                                        available: false
                                    })
                                captureMode: rowItem.modelData.section === "window" ? 1 : 0
                                width: rowItem.modelData.section === "window" ? root.windowsCardWidth : root.displaysCardWidth
                                height: rowItem.modelData.section === "window" ? root.windowsCardHeight : root.displaysCardHeight
                            }
                        }
                    }

                    Loader {
                        active: rowItem.modelData.kind === "region"
                        anchors.fill: parent
                        sourceComponent: Component {
                            Column {
                                spacing: ExoTheme.spacingMd
                                Label {
                                    objectName: "regionCaption"
                                    width: parent.width
                                    text: {
                                        const anchor = root.regionAnchorRow();
                                        return anchor ? qsTr("A preset starts an editable rectangle on %1.").arg(anchor.regionLabel) : qsTr("Select a display first.");
                                    }
                                    wrapMode: Text.WordWrap
                                    color: ExoTheme.textSecondary
                                    font.family: ExoTheme.sansFamily
                                    font.pixelSize: ExoTheme.fontSecondary
                                }
                                Flow {
                                    width: parent.width
                                    spacing: ExoTheme.spacingSm
                                    Repeater {
                                        model: root.regionPresetRows
                                        delegate: PresetCard {
                                            width: Math.min(190, (rowItem.width - 2 * ExoTheme.spacingSm) / 3)
                                            height: 128
                                        }
                                    }
                                }
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

    Popup {
        id: rulesPopup
        objectName: "hiddenRulesPopup"
        parent: Overlay.overlay
        modal: true
        focus: true
        width: Math.min(440, root.safeWidth - 48)
        height: Math.min(360, root.safeHeight - 48)
        x: (root.safeWidth - width) / 2
        y: (root.safeHeight - height) / 2
        padding: ExoTheme.spacingMd

        contentItem: ColumnLayout {
            Label {
                text: qsTr("Hidden window rules")
                font.family: ExoTheme.sansFamily
                font.pixelSize: ExoTheme.fontSectionTitle
                color: ExoTheme.text
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ColumnLayout {
                    width: rulesPopup.availableWidth
                    Repeater {
                        model: root.hiddenWindowRules
                        delegate: RowLayout {
                            id: ruleRow
                            required property var modelData
                            objectName: "hiddenRule-" + modelData.id
                            Layout.fillWidth: true
                            ExoCheckBox {
                                checked: ruleRow.modelData.enabled
                                Accessible.name: qsTr("Enable rule %1").arg(ruleRow.modelData.text)
                                onToggledByUser: value => root.recordViewModel.setHiddenRuleEnabled(ruleRow.modelData.id, value)
                            }
                            Label {
                                Layout.fillWidth: true
                                text: ruleRow.modelData.kind === "application" ? qsTr("Application: %1 (%2)").arg(ruleRow.modelData.applicationScope).arg(ruleRow.modelData.affectedCount) : qsTr("Title: %1 (%2)").arg(ruleRow.modelData.text).arg(ruleRow.modelData.affectedCount)
                                elide: Text.ElideMiddle
                                color: ExoTheme.text
                            }
                            ExoButton {
                                text: qsTr("Remove")
                                quiet: true
                                onClicked: recordViewModel.removeHiddenRule(ruleRow.modelData.id)
                            }
                        }
                    }
                }
            }

            ExoButton {
                text: qsTr("Done")
                Layout.alignment: Qt.AlignRight
                onClicked: rulesPopup.close()
            }
        }
    }

    Dialog {
        id: titleRuleDialog
        parent: Overlay.overlay
        modal: true
        anchors.centerIn: parent
        title: qsTr("Hide windows by title")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: {
            titleMatch.value = "contains";
            titleCase.checked = false;
            allApplications.checked = false;
        }
        onAccepted: {
            if (root.hideCandidate)
                recordViewModel.addTitleRule(root.hideCandidate.identity, titlePattern.value, titleMatch.value, titleCase.checked, allApplications.checked);
        }
        contentItem: ColumnLayout {
            ExoTextField {
                id: titlePattern
                Layout.preferredWidth: 320
                placeholderText: qsTr("Window title text")
                onTextEdited: titlePattern.value = titlePattern.text
            }
            ExoSelect {
                id: titleMatch
                Layout.fillWidth: true
                options: [
                    {
                        value: "equals",
                        label: qsTr("Equals")
                    },
                    {
                        value: "contains",
                        label: qsTr("Contains")
                    }
                ]
                value: "contains"
                onValueActivated: value => titleMatch.value = value
            }
            ExoCheckBox {
                id: titleCase
                text: qsTr("Match case")
            }
            ExoCheckBox {
                id: allApplications
                text: qsTr("Match all applications")
            }
        }
    }

    Dialog {
        id: applicationRuleDialog
        parent: Overlay.overlay
        modal: true
        anchors.centerIn: parent
        title: qsTr("Hide application")
        standardButtons: Dialog.Yes | Dialog.Cancel
        onAccepted: {
            if (root.hideCandidate)
                recordViewModel.addApplicationRule(root.hideCandidate.identity, true);
        }
        contentItem: Label {
            width: 320
            wrapMode: Text.WordWrap
            text: root.hideCandidate ? qsTr("Hide all windows from %1?").arg(root.hideCandidate.appName) : ""
        }
    }

    component TargetCard: Rectangle {
        id: card

        // A QVariantList model exposes one role, modelData, whose map keys do
        // not initialize per-key required properties -- so the row arrives as
        // modelData and the typed surface is derived from it.
        required property var modelData

        readonly property int targetIndex: card.modelData.targetIndex
        readonly property string identity: card.modelData.identity
        onIdentityChanged: if (card.activeFocus)
            root.contentItem.forceActiveFocus(Qt.OtherFocusReason)
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
        readonly property string secondaryLabel: card.modelData.available === false ? card.modelData.unavailableReason : card.kind === "window" && card.windowTitle !== "" ? card.appName : ""
        readonly property var still: root.recordViewModel.targetStillOptions[card.identity]
        readonly property string thumbnailState: card.still ? card.still.state : "placeholder"
        readonly property string thumbnailSource: card.still ? card.still.source : ""

        property int captureMode: 0
        readonly property bool pending: root.pendingIdentity !== "" && card.identity === root.pendingIdentity && root.pendingKind === card.kind && root.pendingCaptureMode === card.captureMode

        enabled: card.modelData.available !== false && card.identity !== ""
        opacity: card.enabled ? 1 : ExoTheme.disabledOpacity
        objectName: "targetCard-" + card.identity
        color: card.pending ? ExoTheme.accentTint(ExoTheme.surfaceRaised, 0.14) : cardHover.hovered ? ExoTheme.surfaceHover : ExoTheme.surface
        border.width: card.pending ? 2 : 1
        border.color: card.pending ? ExoTheme.accent : card.activeFocus ? ExoTheme.text : cardHover.hovered ? ExoTheme.lineStrong : ExoTheme.line
        radius: ExoTheme.radiusSm
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: card.label

        Rectangle {
            objectName: "cardFocusRing"
            anchors.fill: parent
            anchors.margins: -3
            visible: parent.activeFocus
            color: "transparent"
            border.width: ExoTheme.focusRingWidth
            border.color: ExoTheme.text
            radius: ExoTheme.radiusSm + 3
        }

        Keys.onReturnPressed: event => {
            root.confirmCard(card.modelData, card.captureMode);
            event.accepted = true;
        }
        Keys.onEnterPressed: event => {
            root.confirmCard(card.modelData, card.captureMode);
            event.accepted = true;
        }
        Keys.onSpacePressed: event => {
            root.selectCard(card.modelData, card.captureMode);
            event.accepted = true;
        }

        HoverHandler {
            id: cardHover
            cursorShape: card.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        }

        ToolTip {
            text: card.modelData.available === false ? card.modelData.unavailableReason : card.label
            visible: (cardHover.hovered || card.activeFocus) && card.label !== ""
            delay: 400
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: mouse => {
                if (mouse.button === Qt.LeftButton)
                    root.selectCard(card.modelData, card.captureMode);
            }
            onDoubleClicked: mouse => {
                if (mouse.button === Qt.LeftButton)
                    root.confirmCard(card.modelData, card.captureMode);
            }
            onPressed: mouse => {
                if (mouse.button === Qt.RightButton && card.kind === "window") {
                    root.hideCandidate = card.modelData;
                    hideMenu.popup();
                }
            }
        }

        ExoMenu {
            id: hideMenu
            ExoMenuItem {
                text: qsTr("Hide this window until restart")
                onTriggered: root.hideCandidateSession()
            }
            ExoMenuItem {
                text: qsTr("Hide windows by title...")
                enabled: card.modelData.applicationIdentity !== ""
                onTriggered: {
                    titlePattern.value = card.windowTitle;
                    titleRuleDialog.open();
                }
            }
            ExoMenuItem {
                text: qsTr("Hide this application...")
                enabled: card.modelData.applicationIdentity !== ""
                onTriggered: applicationRuleDialog.open()
            }
        }

        ToolButton {
            objectName: "windowHideOptions"
            visible: card.kind === "window"
            text: "\u22ee"
            Accessible.name: qsTr("Window hide options")
            anchors.top: parent.top
            anchors.right: parent.right
            onClicked: {
                root.hideCandidate = card.modelData;
                hideMenu.popup();
            }
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
                    maximumLineCount: 1
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
        border.color: presetCard.pendingPreset ? ExoTheme.accent : presetCard.emphasized ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: presetCard.label

        Rectangle {
            objectName: "cardFocusRing"
            anchors.fill: parent
            anchors.margins: -3
            visible: parent.activeFocus
            color: "transparent"
            border.width: ExoTheme.focusRingWidth
            border.color: ExoTheme.text
            radius: ExoTheme.radiusSm + 3
        }

        Keys.onReturnPressed: event => {
            root.commitPreset(presetCard.key);
            event.accepted = true;
        }
        Keys.onEnterPressed: event => {
            root.commitPreset(presetCard.key);
            event.accepted = true;
        }
        Keys.onSpacePressed: event => {
            root.pendingPresetKey = presetCard.key;
            root.pendingKind = "region";
            event.accepted = true;
        }

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                root.pendingPresetKey = presetCard.key;
                root.pendingKind = "region";
            }
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
