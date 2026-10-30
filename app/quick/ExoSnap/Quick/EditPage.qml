pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

FocusScope {
    id: root
    required property EditSessionAdapter session
    required property EditTimelineAdapter timeline
    required property EditPlayerAdapter player
    required property EditExportAdapter exporter
    required property RecordViewModelAdapter recordings
    required property ShellAdapter shell
    property bool mediaOpen: false
    // The right host is open; rightContext says which of its three contexts
    // (details, mix, export) it shows. Pin and width belong to the host.
    property bool detailsOpen: false
    property string rightContext: "details"
    readonly property real railWidth: 40
    readonly property real stageWidth: stage.width - 2 * railWidth
    readonly property bool exportVisible: detailsOpen && rightContext === "export"
    readonly property bool canOpenExport: session.durationUs > 0 || exporter.state !== EditExportAdapter.Options
    property bool mediaPinPreferred: layoutPreferences.mediaPinned
    property bool detailsPinPreferred: layoutPreferences.detailsPinned
    property bool mediaPinnedLast: layoutPreferences.mediaPinnedLast
    property int sourceTab: 0
    property Item mediaInvoker: null
    property Item detailsInvoker: null
    readonly property real maximumPanelWidth: Math.max(240, Math.min(560, width * 0.45))
    readonly property real mediaWidth: Math.max(240, Math.min(maximumPanelWidth, layoutPreferences.mediaWidth))
    readonly property real detailsWidth: Math.max(240, Math.min(maximumPanelWidth, layoutPreferences.detailsWidth))
    readonly property real panelWidth: mediaWidth
    readonly property bool docksAllowed: width >= 1000
    readonly property bool bothDocksFit: stageWidth - mediaWidth - detailsWidth - 16 >= 480
    readonly property bool mediaPinned: mediaOpen && mediaPinPreferred && docksAllowed
        && stageWidth - mediaWidth - 8 >= 480
        && (bothDocksFit || !detailsOpen || !detailsPinPreferred || !mediaPinnedLast)
    readonly property bool detailsPinned: detailsOpen && detailsPinPreferred && docksAllowed
        && stageWidth - detailsWidth - 8 >= 480
        && (bothDocksFit || !mediaOpen || !mediaPinPreferred || mediaPinnedLast)
    readonly property bool canPinMedia: docksAllowed && stageWidth - mediaWidth - 8 - (detailsPinned ? detailsWidth + 8 : 0) >= 480
    readonly property bool canPinDetails: docksAllowed && stageWidth - detailsWidth - 8 - (mediaPinned ? mediaWidth + 8 : 0) >= 480
    onExportVisibleChanged: if (exportVisible) exporter.markResultSeen()
    Connections {
        target: root.exporter
        function onResultChanged(): void {
            if (root.exportVisible && root.exporter.resultUnseen)
                root.exporter.markResultSeen();
        }
    }
    onMediaPinPreferredChanged: layoutPreferences.mediaPinned = mediaPinPreferred
    onDetailsPinPreferredChanged: layoutPreferences.detailsPinned = detailsPinPreferred
    onMediaPinnedLastChanged: layoutPreferences.mediaPinnedLast = mediaPinnedLast
    objectName: "quickEditPage"

    function revealRequestedRecording(): void {
        if (!root.visible || root.recordings.pendingRecordingIdentity === "")
            return;
        root.sourceTab = 2;
        // Revealing media must not commit or destroy an editor draft.
        if (!root.mediaOpen) {
            root.mediaInvoker = root.Window.window ? root.Window.window.activeFocusItem : clips;
            root.mediaOpen = true;
        }
    }
    onVisibleChanged: if (visible) root.revealRequestedRecording()
    Component.onCompleted: {
        root.restoreVertical();
        root.revealRequestedRecording();
    }
    Connections {
        target: root.recordings
        function onRecordingRevealRequested(): void { root.revealRequestedRecording(); }
    }
    Keys.priority: Keys.AfterItem
    Keys.forwardTo: [clips]
    Keys.onPressed: event => {
        const focused = root.Window.window ? root.Window.window.activeFocusItem : null;
        const draft = focused as EditDraftField;
        if ((focused instanceof TextInput || focused instanceof TextEdit)
                && !(event.key === Qt.Key_Escape && draft && !draft.draftActive))
            return;
        if (event.key === Qt.Key_Escape && (root.detailsOpen || root.mediaOpen)) {
            if (root.detailsOpen) {
                root.toggleRight(root.rightContext, true);
                event.accepted = !root.detailsOpen;
            } else {
                root.toggleMedia(true);
                event.accepted = !root.mediaOpen;
            }
        } else if (!event.isAutoRepeat && event.modifiers === Qt.ControlModifier && event.key === Qt.Key_M) {
            root.toggleMedia(true);
            event.accepted = true;
        } else if (!event.isAutoRepeat && event.modifiers === Qt.ControlModifier && event.key === Qt.Key_D) {
            root.toggleDetails(true);
            event.accepted = true;
        } else if (!event.isAutoRepeat && event.modifiers === Qt.ControlModifier && event.key === Qt.Key_E) {
            root.openExport();
            event.accepted = true;
        }
    }
    Binding {
        target: root.timeline
        property: "trackWidth"
        value: clips.width
    }
    // The panel layout as this page decided it, for the control channel. The
    // page keeps the overlay and pin rules; requests below go through the same
    // toggles as the footer buttons and shortcuts.
    Binding {
        target: root.shell
        property: "editPanels"
        value: ({
                mediaOpen: root.mediaOpen,
                mediaPinned: root.mediaPinned,
                mediaCanPin: root.canPinMedia,
                detailsOpen: root.detailsOpen,
                rightContext: root.rightContext,
                detailsPinned: root.detailsPinned,
                detailsCanPin: root.canPinDetails,
                sourceTab: root.sourceTab
            })
    }
    Connections {
        target: root.shell
        function onEditPanelRequested(panel: int, open: bool, pinned: bool): void {
            const media = panel === ShellAdapter.EditMediaPanel;
            if (media && root.mediaOpen !== open)
                root.toggleMedia();
            else if (!media && (root.detailsOpen !== open || (open && root.rightContext !== "details")))
                root.toggleDetails();
            if (!open)
                return;
            if (media && root.mediaPinned !== pinned && (!pinned || root.canPinMedia))
                root.setPanelPinned(true, pinned);
            else if (!media && root.detailsPinned !== pinned && (!pinned || root.canPinDetails))
                root.setPanelPinned(false, pinned);
        }
        function onEditSourceTabRequested(tab: int): void {
            root.sourceTab = tab;
        }
    }
    Connections {
        target: root.session
        function onTimeSelectionChanged(): void {
            root.exporter.setTimeSelectionUs(root.session.timeSelectionInUs, root.session.timeSelectionOutUs);
        }
        function onDirectoryDropRequested(requestId: var, roots: var, candidates: var, atUs: var, reason: string): void {
            if (directoryChoice.requestId !== 0)
                root.session.cancelMediaDrop(directoryChoice.requestId);
            directoryChoice.requestId = requestId;
            directoryChoice.candidateCount = candidates.length;
            directoryChoice.rootCount = roots.length;
            directoryChoice.reason = reason;
            directoryChoice.open();
        }
    }
    function toggleMedia(keyboardActivation: var): void {
        if (!root.session.preflightPendingDraft())
            return;
        if (!root.mediaOpen)
            root.mediaInvoker = root.Window.window ? root.Window.window.activeFocusItem : clips;
        root.mediaOpen = !root.mediaOpen;
        if (!root.mediaOpen) {
            if (keyboardActivation === true)
                (root.mediaInvoker || clips).forceActiveFocus(Qt.TabFocusReason);
            else
                clips.forceActiveFocus(Qt.MouseFocusReason);
        }
        if (root.mediaOpen && root.width < 1200 && !root.detailsPinned) root.detailsOpen = false;
    }
    // A panel context switch in a rail. Remembers whether it was activated from
    // the keyboard so closing can hand focus back to where it came from.
    component RailButton: EditActionButton {
        id: rail
        property bool keyboardActivation: false
        signal activated(bool keyboard)
        implicitWidth: 32
        panelAccess: true
        onClicked: rail.activated(rail.keyboardActivation)
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                rail.keyboardActivation = true;
        }
        PointHandler {
            acceptedButtons: Qt.LeftButton
            onActiveChanged: if (active) rail.keyboardActivation = false
        }
    }
    component EditingToggle: ExoMenuItem {
        id: toggle
        checkable: true
        leftPadding: 32
        indicator: ExoGlyph {
            x: 10
            y: (toggle.height - height) / 2
            width: 14; height: 14
            kind: ExoGlyph.Check
            color: ExoTheme.accent
            visible: toggle.checked
        }
    }
    function toggleDetails(keyboardActivation: var): void {
        root.toggleRight("details", keyboardActivation);
    }
    // A rail button or shortcut for the context already shown closes the host;
    // any other context replaces it in place, keeping pin and width.
    function toggleRight(context: string, keyboardActivation: var): void {
        if (!root.session.preflightPendingDraft())
            return;
        const closing = root.detailsOpen && root.rightContext === context;
        if (!closing && context === "export" && !root.canOpenExport)
            return;
        if (!root.detailsOpen)
            root.detailsInvoker = root.Window.window ? root.Window.window.activeFocusItem : clips;
        // Resolved before the host unloads: an invoker inside the closing panel
        // is about to be destroyed, and one that has been hidden since cannot
        // take focus, so keyboard focus would otherwise land nowhere.
        const returnTo = closing && keyboardActivation === true ? root.keyboardCloseTarget(context) : null;
        if (!closing)
            root.rightContext = context;
        root.detailsOpen = !closing;
        if (closing) {
            if (returnTo)
                returnTo.forceActiveFocus(Qt.TabFocusReason);
            else
                clips.forceActiveFocus(Qt.MouseFocusReason);
        } else if (keyboardActivation === true && context !== "details") {
            Qt.callLater(() => root.rightContextFocusRequested());
        }
        if (root.detailsOpen && root.width < 1200 && !root.mediaPinned) root.mediaOpen = false;
    }
    signal rightContextFocusRequested()
    // Keyboard close returns to whatever opened the panel, so Space keeps
    // driving the transport after Ctrl+D or Ctrl+E from the timeline. Without a
    // usable invoker the context's rail button takes focus.
    function keyboardCloseTarget(context: string): Item {
        let invoker = root.detailsInvoker;
        for (let item = invoker; item; item = item.parent)
            if (item === rightPanelHost)
                invoker = null;
        if (invoker && invoker.visible && invoker.enabled && invoker.Window.window === root.Window.window)
            return invoker;
        const rail = context === "mix" ? mixRail : context === "export" ? exportButton : detailsRail;
        return rail.enabled ? rail : clips;
    }
    function openInspector(field: string): void {
        if (!root.session.preflightPendingDraft())
            return;
        if (!root.detailsOpen || root.rightContext !== "details")
            root.toggleDetails();
        if (root.detailsOpen && field.length > 0)
            Qt.callLater(() => root.inspectorFieldRequested(field));
    }
    signal inspectorFieldRequested(string fieldName)
    function setPanelPinned(media: bool, pinned: bool): void {
        if (!root.session.preflightPendingDraft())
            return;
        if (media) root.mediaPinPreferred = pinned;
        else root.detailsPinPreferred = pinned;
        if (pinned) root.mediaPinnedLast = media;
    }
    function resizePanel(media: bool, value: real): void {
        const bounded = Math.max(240, Math.min(root.maximumPanelWidth, value));
        if (media) layoutPreferences.mediaWidth = bounded;
        else layoutPreferences.detailsWidth = bounded;
    }
    function openExport(): void {
        root.toggleRight("export", true);
    }
    function openSourceChoice(): void {
        if (root.session.preflightPendingDraft())
            sourceChoice.open();
    }
    component DockResize: Rectangle {
        id: divider
        required property bool media
        property real initialWidth: 300
        width: 8
        color: activeFocus ? ExoTheme.accent : pointer.containsMouse || pointer.pressed ? ExoTheme.lineStrong : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.Separator
        Accessible.name: media ? qsTr("Resize Media. Use Left and Right.") : qsTr("Resize Details. Use Left and Right.")
        Keys.onPressed: event => {
            if (event.key !== Qt.Key_Left && event.key !== Qt.Key_Right && event.key !== Qt.Key_Home)
                return;
            if (!root.session.preflightPendingDraft()) {
                event.accepted = true;
                return;
            }
            const step = (event.modifiers & Qt.ShiftModifier) !== 0 ? 32 : 8;
            const current = divider.media ? root.mediaWidth : root.detailsWidth;
            root.resizePanel(divider.media, event.key === Qt.Key_Home ? 300 : current + (event.key === Qt.Key_Right ? step : -step));
            event.accepted = true;
        }
        MouseArea {
            id: pointer
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.SplitHCursor
            onPressed: mouse => {
                if (!root.session.preflightPendingDraft()) {
                    mouse.accepted = false;
                    return;
                }
                divider.initialWidth = divider.media ? root.mediaWidth : root.detailsWidth;
                divider.forceActiveFocus(Qt.MouseFocusReason);
                startX = mapToItem(stage, mouse.x, mouse.y).x;
            }
            property real startX: 0
            onPositionChanged: mouse => {
                if (pressed) {
                    const delta = mapToItem(stage, mouse.x, mouse.y).x - startX;
                    root.resizePanel(divider.media, divider.initialWidth + (divider.media ? delta : -delta));
                }
            }
            onDoubleClicked: if (root.session.preflightPendingDraft()) root.resizePanel(divider.media, 300)
        }
    }
    function restoreVertical(): void {
        if (!zones.resizing)
            lowerZone.SplitView.preferredHeight = Math.max(220, Math.min(zones.height - 170, Math.max(0, zones.height - 10) * root.fraction(layoutPreferences.timelineFraction, 0.37)));
    }
    function fraction(value: real, fallback: real): real {
        return Number.isFinite(value) && value > 0 && value < 1 ? value : fallback;
    }
    // Panels arrive through the shell. A capture must reject requests that did
    // not reach their actual destination instead of saving a plausible fallback.
    function applyHarnessView(layout: var): bool {
        if (!layout || typeof layout !== "object")
            return false;
        const keys = ["fit", "media", "details", "sourceTab", "popup", "viewFromUs", "viewToUs"];
        if (Object.keys(layout).some(key => keys.indexOf(key) < 0))
            return false;
        for (const panel of ["media", "details"]) {
            if (layout[panel] === undefined)
                continue;
            if (["closed", "open", "pinned"].indexOf(layout[panel]) < 0)
                return false;
            if ((panel === "media" ? root.mediaOpen : root.detailsOpen) !== (layout[panel] !== "closed"))
                return false;
        }
        if (layout.sourceTab !== undefined && (!Number.isInteger(layout.sourceTab)
                || layout.sourceTab < 0 || layout.sourceTab > 2 || root.sourceTab !== layout.sourceTab))
            return false;
        if (layout.fit !== undefined && typeof layout.fit !== "boolean")
            return false;
        const hasInterval = layout.viewFromUs !== undefined || layout.viewToUs !== undefined;
        if (hasInterval && (!Number.isFinite(layout.viewFromUs) || !Number.isFinite(layout.viewToUs)
                || layout.viewFromUs < 0 || layout.viewToUs <= layout.viewFromUs))
            return false;
        if (layout.popup !== undefined && typeof layout.popup !== "string")
            return false;
        const popup = layout.popup || "";
        if (["", "volume", "crossfade", "source-audio"].indexOf(popup) < 0)
            return false;
        const selected = root.session.selectedJunctionDetails;
        if (popup === "crossfade" && selected.outgoing === undefined)
            return false;
        if (popup === "source-audio" && !root.session.sourceAudioChoice.active)
            return false;
        if (hasInterval)
            clips.view.setVisibleInterval(layout.viewFromUs, layout.viewToUs);
        else if (layout.fit)
            clips.fitTimeline();
        if (popup === "volume")
            volumePopup.open();
        else if (popup === "crossfade")
            clips.openTransitionPopup(selected);
        else if (popup === "source-audio")
            sourceChoice.open();
        return true;
    }
    Settings {
        id: layoutPreferences
        objectName: "editLayoutPreferences"
        location: root.session.layoutSettingsLocation
        category: "EditLayout"
        property real sourceFraction: 0.29
        property real timelineFraction: 0.37
        property real mediaWidth: 300
        property real detailsWidth: 300
        property real trackHeaderWidth: 168
        property bool mediaPinned: false
        property bool detailsPinned: false
        property bool mediaPinnedLast: true
    }
    SplitView {
        id: zones
        objectName: "editVerticalSplit"
        anchors.fill: parent
        anchors.margins: ExoTheme.spacingSm
        orientation: Qt.Vertical
        onHeightChanged: root.restoreVertical()
        onResizingChanged: if (!resizing && height > 10)
            layoutPreferences.timelineFraction = lowerZone.height / (height - 10)
        handle: Rectangle {
            objectName: "editVerticalSplitHandle"
            implicitHeight: 10
            color: SplitHandle.pressed || SplitHandle.hovered || activeFocus ? ExoTheme.line : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Separator
            Accessible.name: qsTr("Resize preview and timeline. Use Up and Down.")
            HoverHandler { cursorShape: Qt.SplitVCursor }
            Keys.onUpPressed: {
                layoutPreferences.timelineFraction = Math.min(0.8, layoutPreferences.timelineFraction + 0.03);
                root.restoreVertical();
            }
            Keys.onDownPressed: {
                layoutPreferences.timelineFraction = Math.max(0.2, layoutPreferences.timelineFraction - 0.03);
                root.restoreVertical();
            }
        }
        Item {
            id: stage
            objectName: "editViewerStage"
            SplitView.fillHeight: true
            SplitView.minimumHeight: 160
            onWidthChanged: {
                if (root.width < 1200 && root.mediaOpen && root.detailsOpen && !root.mediaPinned && !root.detailsPinned)
                    root.detailsOpen = false;
            }
            EditPlayer {
                anchors.fill: parent
                anchors.leftMargin: root.railWidth + (root.mediaPinned ? root.mediaWidth + 8 : 0)
                anchors.rightMargin: root.railWidth + (root.detailsPinned ? root.detailsWidth + 8 : 0)
                session: root.session
                player: root.player
                onChooseAudioRequested: root.openSourceChoice()
                onOpenMedia: {
                    root.sourceTab = 1;
                    if (!root.mediaOpen) root.toggleMedia();
                }
            }
            Loader {
                id: mediaPanel
                onLoaded: root.revealRequestedRecording()
                objectName: "editMediaPanelLoader"
                active: root.mediaOpen
                width: root.mediaWidth
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.leftMargin: root.railWidth
                sourceComponent: EditSidePanel {
                    objectName: "editMediaPanel"
                    title: qsTr("Media")
                    pinned: root.mediaPinned
                    pinPreferred: root.mediaPinPreferred
                    canPin: root.canPinMedia
                    pinObjectName: "editMediaPin"
                    closeText: qsTr("Close Media")
                    showHeader: false
                    onPinToggled: root.setPanelPinned(true, !root.mediaPinPreferred)
                    onCloseRequested: keyboardActivation => root.toggleMedia(keyboardActivation)
                    EditSourceBrowser {
                        anchors.fill: parent
                        session: root.session
                        recordings: root.recordings
                        timeline: root.timeline
                        currentTab: root.sourceTab
                        pinned: root.mediaPinned
                        pinPreferred: root.mediaPinPreferred
                        canPin: root.canPinMedia
                        onPinToggled: root.setPanelPinned(true, !root.mediaPinPreferred)
                        onCloseRequested: keyboardActivation => root.toggleMedia(keyboardActivation)
                        onCurrentTabChanged: root.sourceTab = currentTab
                    }
                }
            }
            DockResize {
                objectName: "editMediaResize"
                media: true
                visible: root.mediaOpen
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                x: root.railWidth + root.mediaWidth - width / 2
                z: 2
            }
            Loader {
                id: rightPanelHost
                objectName: "editDetailsPanelLoader"
                active: root.detailsOpen
                width: root.detailsWidth
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.rightMargin: root.railWidth
                sourceComponent: EditSidePanel {
                    objectName: "editDetailsPanel"
                    readonly property bool mix: root.rightContext === "mix"
                    readonly property bool exporting: root.rightContext === "export"
                    title: mix ? qsTr("Audio mix") : exporting ? qsTr("Export") : qsTr("Details")
                    glyph: mix ? ExoGlyph.Mixer : exporting ? ExoGlyph.Send : ExoGlyph.Info
                    subtitle: exporting ? exportPanel.stateText() : ""
                    pinned: root.detailsPinned
                    pinPreferred: root.detailsPinPreferred
                    canPin: root.canPinDetails
                    pinObjectName: "editDetailsPin"
                    closeText: mix ? qsTr("Close Audio mix") : exporting ? qsTr("Close Export") : qsTr("Close Details")
                    Accessible.role: Accessible.Pane
                    Accessible.name: title
                    onPinToggled: root.setPanelPinned(false, !root.detailsPinPreferred)
                    onCloseRequested: keyboardActivation => root.toggleRight(root.rightContext, keyboardActivation)
                    EditInspector {
                        id: inspector
                        anchors.fill: parent
                        visible: root.rightContext === "details"
                        session: root.session
                        canvasFocusItem: clips
                        Connections {
                            target: root
                            function onInspectorFieldRequested(fieldName: string): void { inspector.focusField(fieldName); }
                        }
                    }
                    EditMixPanel {
                        id: mixPanel
                        anchors.fill: parent
                        visible: root.rightContext === "mix"
                        session: root.session
                        canvasFocusItem: clips
                    }
                    EditExportPanel {
                        id: exportPanel
                        anchors.fill: parent
                        visible: root.rightContext === "export"
                        session: root.session
                        exporter: root.exporter
                    }
                    Connections {
                        target: root
                        function onRightContextFocusRequested(): void {
                            if (root.rightContext === "mix") mixPanel.focusFirst();
                            else if (root.rightContext === "export") exportPanel.focusFirst();
                        }
                    }
                }
            }
            DockResize {
                objectName: "editDetailsResize"
                media: false
                visible: root.detailsOpen
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                x: parent.width - root.railWidth - root.detailsWidth - width / 2
                z: 2
            }
            Rectangle {
                objectName: "editLeftRail"
                width: root.railWidth
                height: parent.height
                color: ExoTheme.surface
                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: ExoTheme.line
                }
                Column {
                    id: leftRailActions
                    x: 4
                    y: 4
                    spacing: 4
                    RailButton {
                        objectName: "editMediaAccess"
                        text: qsTr("Media")
                        shortcutText: "Ctrl+M"
                        glyph: ExoGlyph.Folder
                        expanded: root.mediaOpen
                        pinned: root.mediaPinned
                        onActivated: keyboard => root.toggleMedia(keyboard)
                    }
                }
            }
            Rectangle {
                objectName: "editRightRail"
                anchors.right: parent.right
                width: root.railWidth
                height: parent.height
                color: ExoTheme.surface
                Rectangle {
                    width: 1
                    height: parent.height
                    color: ExoTheme.line
                }
                Column {
                    id: rightRailActions
                    x: 4
                    y: 4
                    spacing: 4
                    RailButton {
                        id: detailsRail
                        objectName: "editDetailsAccess"
                        text: qsTr("Details")
                        shortcutText: "Ctrl+D"
                        glyph: ExoGlyph.Info
                        expanded: root.detailsOpen && root.rightContext === "details"
                        pinned: expanded && root.detailsPinned
                        onActivated: keyboard => root.toggleRight("details", keyboard)
                    }
                    RailButton {
                        id: mixRail
                        objectName: "editMixAccess"
                        text: qsTr("Audio mix")
                        glyph: ExoGlyph.Mixer
                        expanded: root.detailsOpen && root.rightContext === "mix"
                        pinned: expanded && root.detailsPinned
                        onActivated: keyboard => root.toggleRight("mix", keyboard)
                    }
                    RailButton {
                        id: exportButton
                        readonly property bool failed: root.exporter.state === EditExportAdapter.Failed
                        objectName: "editExport"
                        text: root.exporter.running ? qsTr("Export %1%").arg(root.exporter.progressPercent)
                            : failed ? qsTr("Export failed")
                            : root.exporter.state === EditExportAdapter.Done && root.exporter.resultUnseen ? qsTr("Export complete")
                            : qsTr("Export")
                        shortcutText: "Ctrl+E"
                        glyph: failed ? ExoGlyph.Warning : ExoGlyph.Send
                        glyphColor: failed ? ExoTheme.errorText : ""
                        expanded: root.exportVisible
                        pinned: expanded && root.detailsPinned
                        enabled: root.exportVisible || root.canOpenExport
                        disabledReason: qsTr("The timeline is empty.")
                        onActivated: keyboard => root.toggleRight("export", keyboard)
                        Rectangle {
                            objectName: "editExportRailProgress"
                            x: 4
                            y: parent.height - 3
                            height: 2
                            radius: 1
                            width: (parent.width - 8) * root.exporter.progressPercent / 100
                            visible: root.exporter.running
                            color: ExoTheme.accent
                        }
                        Rectangle {
                            objectName: "editExportRailDot"
                            x: parent.width - 9
                            y: 3
                            width: 6
                            height: 6
                            radius: 3
                            visible: root.exporter.resultUnseen
                            color: exportButton.failed ? ExoTheme.errorText : ExoTheme.accent
                        }
                    }
                }
            }
        }
        Rectangle {
            id: lowerZone
            objectName: "editTimelineZone"
            SplitView.minimumHeight: 220
            SplitView.preferredHeight: 260
            color: ExoTheme.surface
            radius: ExoTheme.radiusMd
            border.color: ExoTheme.line
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Item {
                    id: header
                    objectName: "editTimelineHeader"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(46, actionGroup.implicitHeight + 12, transportGroup.implicitHeight + 12)
                    Row {
                        id: actionGroup
                        objectName: "editActionGroup"
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        EditActionButton { objectName: "editSelectTool"; text: qsTr("Selection tool"); shortcutText: "V"; glyph: ExoGlyph.Pointer; visible: root.width >= 1000; checkable: true; checked: clips.tool === "select"; onClicked: { clips.tool = "select"; checked = Qt.binding(() => clips.tool === "select"); } }
                        EditActionButton { objectName: "editRangeTool"; text: qsTr("Range tool"); shortcutText: "R"; glyph: ExoGlyph.RangeTool; visible: root.width >= 1000; checkable: true; checked: clips.tool === "range"; onClicked: { clips.tool = "range"; checked = Qt.binding(() => clips.tool === "range"); } }
                        EditActionButton { objectName: "editUndo"; text: qsTr("Undo"); shortcutText: "Ctrl+Z"; glyph: ExoGlyph.Undo; enabled: root.session.canUndo; disabledReason: qsTr("There is no edit to undo."); onClicked: root.session.undo() }
                        EditActionButton { objectName: "editRedo"; text: qsTr("Redo"); shortcutText: "Ctrl+Y"; glyph: ExoGlyph.Redo; enabled: root.session.canRedo; disabledReason: qsTr("There is no edit to redo."); onClicked: root.session.redo() }
                        EditActionButton { objectName: "editSplit"; text: qsTr("Split"); shortcutText: "Ctrl+B"; glyph: ExoGlyph.Scissors; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.splitSelected() }
                        EditActionButton { objectName: "editDelete"; text: qsTr("Delete"); shortcutText: "Delete"; glyph: ExoGlyph.Trash; enabled: [EditSessionAdapter.ClipSelection, EditSessionAdapter.RangeSelection, EditSessionAdapter.JunctionSelection, EditSessionAdapter.SpacerSelection].includes(root.session.selectionMode); disabledReason: qsTr("Select clips, a range, a junction or a spacer to delete."); onClicked: root.session.deleteSelected(false) }
                        EditActionButton { objectName: "editRippleDeleteAction"; text: qsTr("Ripple delete"); shortcutText: "Shift+Delete"; glyph: ExoGlyph.CloseGap; visible: root.width >= 1000; enabled: [EditSessionAdapter.ClipSelection, EditSessionAdapter.RangeSelection, EditSessionAdapter.JunctionSelection, EditSessionAdapter.SpacerSelection, EditSessionAdapter.GapSelection].includes(root.session.selectionMode); disabledReason: qsTr("Select clips, a range, a junction, a spacer or a gap to remove."); onClicked: root.session.deleteSelected(true) }
                    }
                    Row {
                        id: transportGroup
                        objectName: "editTransportGroup"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        EditActionButton { objectName: "editTransportStart"; text: qsTr("Timeline start"); glyph: ExoGlyph.JumpStart; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.requestSeekUs(0) }
                        EditActionButton { objectName: "editTransportPreviousFrame"; text: qsTr("Previous frame"); glyph: ExoGlyph.StepBack; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.requestFrameStep(-1) }
                        EditActionButton {
                            objectName: "editTransportPlay"
                            text: root.player.desiredPlaying ? qsTr("Pause") : qsTr("Play")
                            shortcutText: "Space"
                            glyph: root.player.desiredPlaying ? ExoGlyph.Pause : ExoGlyph.Run
                            enabled: root.session.durationUs > 0
                            disabledReason: qsTr("The timeline is empty.")
                            onClicked: root.player.togglePlay()
                        }
                        EditActionButton { objectName: "editTransportNextFrame"; text: qsTr("Next frame"); glyph: ExoGlyph.ArrowRight; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.requestFrameStep(1) }
                        EditActionButton { objectName: "editTransportEnd"; text: qsTr("Timeline end"); glyph: ExoGlyph.Stop; enabled: root.session.durationUs > 0; disabledReason: qsTr("The timeline is empty."); onClicked: root.session.requestSeekUs(root.session.durationUs) }
                        EditActionButton {
                            objectName: "editAudioChoice"
                            text: qsTr("Choose audio tracks")
                            glyph: ExoGlyph.Speaker
                            visible: root.session.sourceAudioChoices.length > 0
                            onClicked: root.openSourceChoice()
                        }
                        EditActionButton {
                            objectName: "editLoop"
                            text: qsTr("Loop")
                            glyph: ExoGlyph.Repeat
                            shortcutText: "Ctrl+L"
                            checkable: true
                            checked: root.player.loopEnabled
                            enabled: root.session.durationUs > 0
                            disabledReason: qsTr("The timeline is empty.")
                            onClicked: root.player.loopEnabled = !root.player.loopEnabled
                        }
                        EditActionButton {
                            objectName: "editRangeOnce"
                            text: qsTr("Play range once")
                            glyph: ExoGlyph.PlayRange
                            shortcutText: "Shift+Space"
                            enabled: root.session.selectionMode === EditSessionAdapter.RangeSelection
                            disabledReason: qsTr("Select a range to play once.")
                            onClicked: root.player.playRangeOnce()
                        }
                        EditTimeField {
                            objectName: "editTransportTimecode"
                            anchors.verticalCenter: parent.verticalCenter
                            width: 132
                            session: root.session
                            canvasFocusItem: clips
                            format: 3
                            valueUs: root.session.positionUs
                            applyValue: value => root.session.applyTimeValue(0, "playhead", value)
                            Accessible.name: qsTr("Playback position")
                        }
                    }
                    Row {
                        id: outputGroup
                        objectName: "editOutputGroup"
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        EditActionButton { objectName: "editSnapping"; text: qsTr("Snapping"); glyph: ExoGlyph.Magnet; visible: root.width >= 1100; checkable: true; checked: root.session.snapping; onClicked: root.session.snapping = checked }
                        EditActionButton { objectName: "editRipple"; text: qsTr("Ripple"); glyph: ExoGlyph.CloseGap; visible: root.width >= 1100; checkable: true; checked: root.session.ripple; onClicked: root.session.ripple = checked }
                        EditActionButton { objectName: "editAutoCrossfade"; text: qsTr("Auto Crossfade"); glyph: ExoGlyph.Layers; visible: root.width >= 1100; checkable: true; checked: root.session.autoCrossfade; onClicked: root.session.autoCrossfade = checked }
                        EditActionButton { objectName: "editLinkedSelection"; text: qsTr("Linked Selection"); glyph: ExoGlyph.SplitTrack; visible: root.width >= 1100; checkable: true; checked: root.session.linkedSelection; onClicked: root.session.linkedSelection = checked }
                        EditActionButton {
                            objectName: "editToolbarOverflow"
                            text: qsTr("More editing actions")
                            glyph: ExoGlyph.Overflow
                            onClicked: editOverflow.popup()
                            ExoMenu {
                                id: editOverflow
                                EditingToggle { objectName: "editOverflowSelectTool"; text: qsTr("Selection tool"); visible: root.width < 1000; checked: clips.tool === "select"; onTriggered: clips.tool = "select" }
                                EditingToggle { objectName: "editOverflowRangeTool"; text: qsTr("Range tool"); visible: root.width < 1000; checked: clips.tool === "range"; onTriggered: clips.tool = "range" }
                                EditingToggle { text: qsTr("Snapping"); visible: root.width < 1100; checked: root.session.snapping; onTriggered: root.session.snapping = !root.session.snapping }
                                EditingToggle { text: qsTr("Ripple"); visible: root.width < 1100; checked: root.session.ripple; onTriggered: root.session.ripple = !root.session.ripple }
                                EditingToggle { text: qsTr("Auto Crossfade"); visible: root.width < 1100; checked: root.session.autoCrossfade; onTriggered: root.session.autoCrossfade = !root.session.autoCrossfade }
                                EditingToggle { objectName: "editOverflowLinkedSelection"; text: qsTr("Linked Selection"); visible: root.width < 1100; checked: root.session.linkedSelection; onTriggered: root.session.linkedSelection = !root.session.linkedSelection }
                                ExoMenuItem { text: qsTr("Ripple delete"); visible: root.width < 1000; enabled: root.session.selectionMode !== EditSessionAdapter.NoSelection; onTriggered: root.session.deleteSelected(true) }
                                ExoMenuItem { text: qsTr("Fit selection"); onTriggered: clips.fitSelection() }
                                ExoMenuItem {
                                    objectName: "editFitClips"
                                    text: qsTr("Fit clips")
                                    onTriggered: {
                                        const bounds = root.session.occupiedBounds();
                                        if (bounds.valid) clips.view.fitOccupied(bounds.startUs, bounds.endUs);
                                        else clips.fitTimeline();
                                    }
                                }
                                ExoMenuItem { text: qsTr("Editor shortcuts"); onTriggered: shortcutHelp.open() }
                            }
                        }
                        EditActionButton {
                            id: volumeButton
                            objectName: "editVolume"
                            text: root.player.volume === 0 ? qsTr("Preview volume (muted)") : qsTr("Preview volume")
                            glyph: ExoGlyph.Speaker
                            onClicked: volumePopup.open()
                            Popup {
                                id: volumePopup
                                objectName: "editVolumePopup"
                                y: -height - 4
                                x: volumeButton.width - width
                                width: 180
                                padding: ExoTheme.spacingMd
                                focus: true
                                background: Rectangle {
                                    color: ExoTheme.surfaceRaised
                                    border.width: 1
                                    border.color: ExoTheme.lineStrong
                                    radius: ExoTheme.radiusMd
                                }
                                onOpened: volumeSlider.forceActiveFocus(Qt.PopupFocusReason)
                                onClosed: volumeButton.forceActiveFocus(Qt.PopupFocusReason)
                                contentItem: ExoSlider {
                                    id: volumeSlider
                                    objectName: "editVolumeSlider"
                                    from: 0
                                    to: 1
                                    stepSize: 0.1
                                    value: root.player.volume
                                    Accessible.name: qsTr("Preview volume")
                                    onMoved: root.player.volume = value
                                }
                            }
                        }
                    }
                }
                Label {
                    objectName: "editDeliveryPending"
                    Layout.fillWidth: true
                    text: qsTr("MP4 delivery in progress. The finalized edit master is ready.")
                    visible: root.session.deliveryPending
                    color: ExoTheme.textSecondary
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    objectName: "editNavigator"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    spacing: 2
                    Label {
                        objectName: "editRangeReadout"
                        Layout.preferredWidth: clips.labelWidth - 8
                        readonly property var range: root.session.editRange
                        text: root.session.selectionMode === EditSessionAdapter.RangeSelection || range.endUs > range.startUs
                            ? qsTr("%1 – %2").arg(root.session.formatTimeValue(range.startUs, 0)).arg(root.session.formatTimeValue(range.endUs, 0))
                            : ""
                        elide: Text.ElideRight
                        color: root.session.selectionMode === EditSessionAdapter.RangeSelection ? ExoTheme.text : ExoTheme.textSecondary
                        font.family: ExoTheme.monoFamily
                        font.pixelSize: ExoTheme.fontCaption
                        Accessible.name: text.length > 0 ? qsTr("Range %1").arg(text) : ""
                    }
                    EditOverview {
                        objectName: "editOverview"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 12
                        session: root.session
                        player: root.player
                        view: clips.view
                        interaction: clips.interaction
                        canvasFocusItem: clips
                    }
                    EditActionButton { objectName: "editZoomOut"; text: qsTr("Zoom out"); glyph: ExoGlyph.Minus; onClicked: clips.zoomTo(clips.pixelsPerSecond / 1.25) }
                    ExoSlider {
                        objectName: "editZoomSlider"
                        Layout.preferredWidth: root.width < 1000 ? 72 : 100
                        from: Math.log(clips.minimumZoom)
                        to: Math.log(clips.maximumZoom)
                        value: Math.log(clips.pixelsPerSecond)
                        Accessible.name: qsTr("Timeline zoom")
                        onMoved: clips.zoomTo(Math.exp(value))
                        onPressedChanged: {
                            if (pressed) clips.view.beginViewOperation();
                            else clips.view.endViewOperation();
                        }
                    }
                    EditActionButton { objectName: "editZoomIn"; text: qsTr("Zoom in"); glyph: ExoGlyph.Plus; onClicked: clips.zoomTo(clips.pixelsPerSecond * 1.25) }
                    EditActionButton { objectName: "editZoomFit"; text: qsTr("Fit timeline"); glyph: ExoGlyph.AppWindow; onClicked: clips.fitTimeline() }
                }
                EditTimeline {
                    id: clips
                    keyboardScope: root
                    session: root.session
                    player: root.player
                    thumbnails: root.timeline
                    userHeaderWidth: layoutPreferences.trackHeaderWidth
                    onHeaderWidthEdited: width => layoutPreferences.trackHeaderWidth = width
                    onExportRequested: root.openExport()
                    onMediaToggleRequested: root.toggleMedia(true)
                    onInspectorToggleRequested: root.toggleDetails(true)
                    onInspectorOpenRequested: fieldName => root.openInspector(fieldName)
                    onShortcutsRequested: shortcutHelp.open()
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 96
                }
                Label {
                    id: selectionStatus
                    objectName: "editSelectionStatus"
                    readonly property string failureText: root.session.rangeGestureActive && !root.session.rangeGestureValid
                        ? root.session.rangeGestureReason : root.session.dragActive && !root.session.dragValid
                            ? root.session.dragReason : root.session.workspaceError
                    // A transient line for refusals and imports, not a permanent
                    // status bar: it takes no room while there is nothing to say.
                    visible: text.length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    color: failureText.length > 0 ? ExoTheme.errorText : ExoTheme.textSecondary
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontSecondary
                    text: {
                        if (failureText.length > 0)
                            return failureText;
                        if (root.session.importPending)
                            return qsTr("Importing media…");
                        return "";
                    }
                    Accessible.name: text
                    HoverHandler { id: statusHover }
                    ToolTip.visible: statusHover.hovered && truncated
                    ToolTip.text: text
                }
            }
        }
    }

    EditSourceChoice { id: sourceChoice; session: root.session }
    Dialog {
        id: shortcutHelp
        title: qsTr("Editor shortcuts")
        modal: true
        focus: true
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 520)
        standardButtons: Dialog.Close
        contentItem: ColumnLayout {
            spacing: ExoTheme.spacingSm
            Repeater {
                model: [
                    qsTr("Space: Play or pause on release"),
                    qsTr("Space + drag / wheel: Pan without changing playback"),
                    qsTr("Shift+Space: Play the active range once"),
                    qsTr("Ctrl+L: Toggle Loop"),
                    qsTr("Left / Right: Step one frame; Alt: one sample; Shift: one second"),
                    qsTr("Up / Down: Previous or next boundary"),
                    qsTr("I / O: Set range in or out; Alt+X: Clear range"),
                    qsTr("Ctrl+B: Split; Delete: Lift; Shift+Delete: Ripple delete"),
                    qsTr("Ctrl+C / X / V: Copy, cut or paste clips; Ctrl+Shift+V: Insert paste"),
                    qsTr("Ctrl+Z / Ctrl+Y: Undo or redo"),
                    qsTr("Z: Fit selection; Shift+Z: Fit timeline; Backspace: Previous view"),
                    qsTr("Ctrl+M / Ctrl+D / Ctrl+E: Media, Inspector or export")
                ]
                Label {
                    required property string modelData
                    Layout.fillWidth: true
                    text: modelData
                    color: ExoTheme.text
                    wrapMode: Text.Wrap
                }
            }
        }
    }
    Dialog {
        id: directoryChoice
        objectName: "editDirectoryDropChoice"
        property var requestId: 0
        property int candidateCount: 0
        property int rootCount: 0
        property string reason: ""
        title: qsTr("Add folders")
        modal: true
        focus: true
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 500)
        function resolve(insert: bool): void {
            const id = requestId;
            requestId = 0;
            root.session.resolveMediaDrop(id, insert);
            close();
        }
        onClosed: {
            if (requestId !== 0) {
                root.session.cancelMediaDrop(requestId);
                requestId = 0;
            }
        }
        contentItem: ColumnLayout {
            spacing: ExoTheme.spacingMd
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 folders contain %2 supported files directly inside them. Subfolders and linked directories are excluded.").arg(directoryChoice.rootCount).arg(directoryChoice.candidateCount)
                wrapMode: Text.Wrap
                color: ExoTheme.text
            }
            Label {
                Layout.fillWidth: true
                visible: directoryChoice.reason.length > 0
                text: directoryChoice.reason
                wrapMode: Text.Wrap
                color: ExoTheme.textSecondary
            }
            ExoButton {
                objectName: "editDirectoryBrowse"
                Layout.fillWidth: true
                text: qsTr("Add browsing roots")
                onClicked: directoryChoice.resolve(false)
            }
            ExoButton {
                objectName: "editDirectoryInsert"
                Layout.fillWidth: true
                text: qsTr("Insert %1 files").arg(directoryChoice.candidateCount)
                enabled: directoryChoice.candidateCount > 0
                onClicked: directoryChoice.resolve(true)
            }
            ExoButton {
                Layout.fillWidth: true
                text: qsTr("Cancel")
                quiet: true
                onClicked: directoryChoice.close()
            }
        }
    }
}
