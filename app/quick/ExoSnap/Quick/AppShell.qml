pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property AboutViewModelAdapter aboutViewModel
    required property RecordViewModelAdapter recordViewModel
    required property RecordPreviewAdapter previewAdapter
    required property SettingsAdapter settingsAdapter
    required property DeviceAdapter deviceAdapter
    required property DiagnosticsAdapter diagnosticsAdapter
    required property LogsAdapter logsAdapter
    required property EditSessionAdapter editSession
    required property EditTimelineAdapter editTimeline
    required property EditPlayerAdapter editPlayer
    required property EditExportAdapter editExport
    required property ShellAdapter shell
    required property NotificationsAdapter notifications
    required property RecoveryAdapter recovery
    required property RecordingErrorAdapter recordingError
    required property CrashReportAdapter crashReport
    required property WhatsNewAdapter whatsNew
    required property ShellPresenceAdapter shellPresence
    property bool benchmarkInteractionActive: false

    // Supplied by Main. Optional so the shell still loads in a QML test or a
    // render harness that has no window chrome to talk to.
    property QuickWindowChrome chrome: null
    property bool windowMaximized: false

    signal minimizeRequested()
    signal maximizeRestoreRequested()
    signal closeRequested()

    // Single source with QuickWindowChrome::kDefaultTitleBarHeight and the
    // Widgets shell's ui::theme::ExoSnapMetrics::kTitlebarHeight.
    readonly property int titleBarHeight: 40
    // Record is the landing destination, matching the Widgets shell. This was an
    // opt-in during the migration, when About was the only migrated page; leaving
    // it that way shipped an application that opens on its own version numbers.
    property int currentPage: ShellAdapter.RecordPage
    // Which Diagnostics view is on screen. Additive state, not a page of its
    // own: Logs moved inside Diagnostics, and a request for the old LogsPage
    // value normalizes to DiagnosticsPage plus this section.
    property int diagnosticsSection: ShellAdapter.DiagnosticsOverview
    readonly property bool editPageVisible: root.displayedPage === ShellAdapter.EditPage

    function stackIndexForPage(page: int): int {
        switch (page) {
        case ShellAdapter.EditPage:
            return 1;
        case ShellAdapter.SettingsPage:
            return 2;
        case ShellAdapter.DiagnosticsPage:
        case ShellAdapter.LogsPage:
            return 3;
        case ShellAdapter.AboutPage:
            return 4;
        default:
            return 0;
        }
    }

    property int displayedPage: ShellAdapter.RecordPage
    readonly property int stackIndex: root.stackIndexForPage(root.displayedPage)

    // Advances `displayedPage` to `currentPage` once its content exists.
    // Called after every navigation and again whenever a loader's status
    // changes, because the destination that was requested is not necessarily
    // the one whose Loader just became ready.
    function refreshDisplayedPage(): void {
        if (root.destinationReady(root.currentPage))
            root.displayedPage = root.currentPage;
    }

    // Loads the destination being navigated to. Written as a switch over the same
    // index space rather than as a generated list: the destinations are a
    // product decision, not a collection.
    //
    // Called from onCurrentPageChanged AND from Component.onCompleted, because a
    // shell constructed with a non-zero currentPage (the --visual-page harness
    // sets it after load, but nothing guarantees that ordering) would otherwise
    // show an empty stack page.
    //
    // setSource(url, properties) rather than an inline sourceComponent: an inline
    // component is part of THIS document, so the engine resolves and compiles the
    // page's type before the first frame even though nothing instantiates it. A
    // URL is a string until it is loaded, so the page documents leave the
    // startup compile entirely (QCR-615). The trade is deliberate: the first
    // deliberate visit to a page pays that page's compile.
    //
    // The properties below are the pages' required adapters. Every one of them is
    // a required property of this shell, handed in once by Main and never
    // reassigned, so an initial value is the whole contract — there is no binding
    // to lose. Signals are the exception: setSource carries values, not handlers,
    // so DiagnosticsWorkspace's two navigation signals are connected separately
    // below.
    //
    // Idempotent by status: a second navigation to the same page finds it loaded
    // and does nothing, which is the resident-page contract QCR-602 established.
    function loadDestination(page: int): void {
        switch (page) {
        case ShellAdapter.EditPage:
            if (editLoader.status === Loader.Null)
                editLoader.setSource(Qt.resolvedUrl("EditPage.qml"), {
                    session: root.editSession, timeline: root.editTimeline,
                    player: root.editPlayer, exporter: root.editExport,
                    recordings: root.recordViewModel
                });
            break;
        case ShellAdapter.SettingsPage:
            if (settingsLoader.status === Loader.Null)
                settingsLoader.setSource(Qt.resolvedUrl("SettingsPage.qml"), {
                    settings: root.settingsAdapter
                });
            break;
        case ShellAdapter.DiagnosticsPage:
            if (diagnosticsLoader.status === Loader.Null)
                diagnosticsLoader.setSource(Qt.resolvedUrl("DiagnosticsWorkspace.qml"), {
                    diagnostics: root.diagnosticsAdapter,
                    device: root.deviceAdapter,
                    logs: root.logsAdapter,
                    shell: root.shell
                });
            break;
        case ShellAdapter.AboutPage:
            if (aboutLoader.status === Loader.Null)
                aboutLoader.setSource(Qt.resolvedUrl("AboutPage.qml"), {
                    aboutViewModel: root.aboutViewModel
                });
            break;
        default:
            break;
        }
    }

    onCurrentPageChanged: {
        if (root.currentPage === ShellAdapter.LogsPage) {
            root.diagnosticsSection = ShellAdapter.DiagnosticsLogs;
            root.currentPage = ShellAdapter.DiagnosticsPage;
            return;
        }
        root.loadDestination(root.currentPage);
        root.refreshDisplayedPage();
    }

    // Whether `page`'s content exists and has finished loading. Record is
    // never loader-built, so it is always ready. LogsPage keeps its own
    // readiness answer: it means "the internal logs view exists", which is what
    // the automation edge has to wait for after a legacy logs navigation.
    //
    // Two independent callers read this through the SAME predicate rather than
    // each re-deriving it: the --visual-test capture (main.cpp) waits for
    // root.currentPage's own readiness before grabbing, and the idle-time
    // pre-warm below polls it for a page that is not even the current one.
    function destinationReady(page: int): bool {
        switch (page) {
        case ShellAdapter.EditPage:
            return editLoader.status === Loader.Ready;
        case ShellAdapter.SettingsPage:
            return settingsLoader.status === Loader.Ready;
        case ShellAdapter.DiagnosticsPage:
            return diagnosticsLoader.status === Loader.Ready && diagnosticsLoader.item.overviewReady;
        case ShellAdapter.LogsPage:
            return diagnosticsLoader.status === Loader.Ready && diagnosticsLoader.item.logsViewReady;
        case ShellAdapter.AboutPage:
            return aboutLoader.status === Loader.Ready;
        default:
            return true;
        }
    }

    // The CURRENT destination's readiness, bound rather than computed once: a
    // switch's dependencies are only the branch actually taken, so this
    // re-resolves correctly both when currentPage changes and when the loader
    // it now reads reaches Ready. The logs view is a subview of Diagnostics, so
    // its own readiness is what a capture of that surface has to wait for.
    readonly property bool activeDestinationReady: {
        if (root.currentPage === ShellAdapter.DiagnosticsPage
                && root.diagnosticsSection === ShellAdapter.DiagnosticsLogs)
            return root.destinationReady(ShellAdapter.LogsPage);
        return root.destinationReady(root.currentPage);
    }

    // Where the shell arrived, published back to C++. Two consumers need it and
    // neither can ask QML: the control channel answers `ui.getState.page` from
    // here instead of from a findChild() on this document's objectName, and the
    // navigation intent below reads it back to report the RESULTING page rather
    // than the requested one.
    //
    // A Binding rather than an assignment in the handler above so the initial
    // value is published too — a shell that starts on a harness-selected page
    // would otherwise report Record until the first navigation.
    Binding {
        target: root.shell
        property: "currentPage"
        value: root.currentPage
    }

    Binding {
        target: root.shell
        property: "diagnosticsSection"
        value: root.diagnosticsSection
    }

    Binding {
        target: root.shell
        property: "editSurfaceVisible"
        value: root.editPageVisible
    }

    function navigateTo(page: int): void {
        if (!root.navigationAllowed)
            return;
        if (page === ShellAdapter.LogsPage) {
            root.diagnosticsSection = ShellAdapter.DiagnosticsLogs;
            root.currentPage = ShellAdapter.DiagnosticsPage;
            return;
        }
        if (page === ShellAdapter.DiagnosticsPage)
            root.diagnosticsSection = ShellAdapter.DiagnosticsOverview;
        root.currentPage = page;
    }

    readonly property bool navigationAllowed: !root.recovery.surfaceOpen && !root.recordingError.active
                                              && !root.crashReport.active && !root.whatsNew.active
                                              && !root.shell.closeGuardActive

    readonly property var navPages: [qsTr("Record"), qsTr("Edit"), qsTr("Settings"), qsTr("Diagnostics"), qsTr("About")]

    readonly property var navPageValues: [ShellAdapter.RecordPage, ShellAdapter.EditPage, ShellAdapter.SettingsPage,
                                          ShellAdapter.DiagnosticsPage, ShellAdapter.AboutPage]

    function pageForTab(index: int): int {
        return root.navPageValues[index] !== undefined ? root.navPageValues[index] : ShellAdapter.RecordPage;
    }

    // Below the regular width class the band gives up tab padding rather than
    // label text or font size: a truncated destination is unreadable and a
    // smaller one breaks the band's single type rung, while 8 px of side padding
    // still leaves every tab a comfortable desktop hit target.
    readonly property bool compactNav: !ExoTheme.isRegular(root.width)

    objectName: "quickAppShell"

    // ── Application shortcuts (QCR-512) ──────────────────────────────────────
    //
    // Scope is the point of this block, not coverage. There are four kinds of
    // key handling in the product and they must not be confused:
    //
    //   global OS hotkeys   registered by Win32HotkeyRegistrar (start/stop/
    //                       pause/marker). They fire while ExoSnap is not even
    //                       focused, they are user-rebindable, and nothing here
    //                       touches them.
    //   window shortcuts    the five below. Ctrl+1..5, the destination order of
    //                       the band above.
    //   surface-local keys  Escape on a modal, the Edit timeline's arrows/I/O,
    //                       the webcam overlay's arrows. They live on the item
    //                       that owns them and only fire while it has focus.
    //   text editing        everything a focused TextField consumes.
    //
    // Ctrl is what keeps the last two apart. QCR-503/504 made many more items
    // real focus targets, so an unmodified letter as an application shortcut
    // would now be a key that types in a hotkey field and navigates everywhere
    // else — the class of bug this item exists to avoid. A modified digit
    // cannot be typed into any field the product has.
    //
    // Disabled rather than merely ineffective while a blocking surface is up.
    // A scrim stops the pointer from reaching the tabs; nothing stops a
    // keystroke, so the keyboard route needs the guard spelled out. It is the
    // same guard `navigateTo()` applies — the binding only keeps the key from
    // being swallowed by a shortcut that would refuse it anyway.
    //
    // An open edit session is NOT part of this (QCR-001). Ctrl+1..5 and the tabs
    // share one contract, and under that contract the edit session is state of
    // the Record destination rather than a surface the user has to answer.
    //
    // Written out rather than generated from `navPages`: five destinations is a
    // product decision (CLAUDE.md), not a list length, and a Repeater of
    // Shortcuts would need a delegate item that exists for nothing else.
    Shortcut {
        sequence: "Ctrl+1"
        context: Qt.WindowShortcut
        enabled: root.navigationAllowed
        onActivated: root.navigateTo(ShellAdapter.RecordPage)
    }

    Shortcut {
        sequence: "Ctrl+2"
        context: Qt.WindowShortcut
        enabled: root.navigationAllowed
        onActivated: root.navigateTo(ShellAdapter.EditPage)
    }

    Shortcut {
        sequence: "Ctrl+3"
        context: Qt.WindowShortcut
        enabled: root.navigationAllowed
        onActivated: root.navigateTo(ShellAdapter.SettingsPage)
    }

    Shortcut {
        sequence: "Ctrl+4"
        context: Qt.WindowShortcut
        enabled: root.navigationAllowed
        onActivated: root.navigateTo(ShellAdapter.DiagnosticsPage)
    }

    Shortcut {
        sequence: "Ctrl+5"
        context: Qt.WindowShortcut
        enabled: root.navigationAllowed
        onActivated: root.navigateTo(ShellAdapter.AboutPage)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Title bar ────────────────────────────────────────────────────────
        //
        // The window's own 40 px band: brand, navigation, notification bell and
        // the three window buttons, with everything between them acting as the
        // drag handle. Windows is told which parts are interactive through
        // QuickWindowChrome; see refreshChromeGeometry().
        Item {
            id: titleBar

            Layout.fillWidth: true
            Layout.preferredHeight: root.titleBarHeight

            // The band is a TOOLBAR, so it takes the surface rung rather than the
            // page ground. In Light that is the whole point of a grey ground: the
            // chrome and the cards are the near-white things, the page is what
            // they sit on. Left at the page colour the band ran straight into the
            // content with only its hairline between them.
            Rectangle {
                anchors.fill: parent
                color: ExoTheme.surface
            }

            // The band's own rects, in shell coordinates. Recomputed whenever
            // anything that can move them changes — the width (every resize),
            // the selected page (the selected tab is bolder and therefore
            // wider), and the bell's unread dot.
            function refreshChromeGeometry(): void {
                if (!root.chrome)
                    return;

                root.chrome.clearInteractiveRects();
                root.chrome.addInteractiveRect(rectInShell(navStrip));
                root.chrome.addInteractiveRect(rectInShell(notificationBell));
                root.chrome.addInteractiveRect(rectInShell(minimizeButton));
                root.chrome.addInteractiveRect(rectInShell(maximizeButton));
                root.chrome.addInteractiveRect(rectInShell(closeButton));

                // Reported separately as well: this one rect returns
                // HTMAXBUTTON rather than HTCLIENT, which is what makes
                // Windows 11 offer the Snap Layouts flyout on hover.
                root.chrome.maximizeButtonRect = rectInShell(maximizeButton);
            }

            function rectInShell(item: Item): rect {
                const origin = item.mapToItem(root, 0, 0);
                return Qt.rect(origin.x, origin.y, item.width, item.height);
            }

            // Deferred: during a resize the layout has not settled when the
            // width change arrives, so reading geometry synchronously would
            // register the rects the band is about to leave.
            onWidthChanged: Qt.callLater(titleBar.refreshChromeGeometry)
            Component.onCompleted: titleBar.refreshChromeGeometry()

            Connections {
                target: root

                function onCurrentPageChanged(): void {
                    Qt.callLater(titleBar.refreshChromeGeometry);
                }

                function onChromeChanged(): void {
                    Qt.callLater(titleBar.refreshChromeGeometry);
                }

                // Maximizing moves every window button by the difference between
                // the restored and the maximized width. Relying on the band's own
                // width change to notice is not enough: the state flip and the
                // resize do not arrive as one event, and a rect left describing
                // the restored window puts the buttons outside every known
                // rectangle, where the hit test answers HTCAPTION and the band
                // drags instead of minimizing or maximizing.
                function onWindowMaximizedChanged(): void {
                    Qt.callLater(titleBar.refreshChromeGeometry);
                }
            }

            RowLayout {
                spacing: ExoTheme.spacingXs
                anchors {
                    fill: parent
                    leftMargin: ExoTheme.spacingLg
                }

                // Mark + wordmark, the same pair the About card and every overlay
                // chrome bar already draw. The shell was the one surface still
                // spelling the product "ExoSnap" in plain body text, which made
                // the band read as a generic window rather than as this product.
                // The mark carries the session, exactly as the tray icon and the
                // taskbar button do and from the same projection: one recording,
                // one state, three surfaces that cannot disagree.
                ExoBrandMark {
                    markState: root.shellPresence.iconState
                    markFrame: root.shellPresence.markFrame
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    Layout.alignment: Qt.AlignVCenter
                    Accessible.ignored: true
                }

                // Artwork rather than text, so the product name cannot be
                // translated, hyphenated, font-substituted, or grown by the
                // text-expansion harness -- which used to put 80 px of pressure
                // on the navigation that no real translation will ever apply.
                ExoBrandWordmark {
                    objectName: "quickBrandWordmark"
                    typePixelSize: ExoTheme.fontBrand
                    Layout.preferredWidth: implicitWidth
                    Layout.preferredHeight: implicitHeight
                    Layout.leftMargin: ExoTheme.spacingSm - ExoTheme.spacingXs
                    // The one gap in the band that separates identity from
                    // navigation, so it is the first thing to give when five
                    // destinations have to fit beside three window buttons.
                    Layout.rightMargin: root.compactNav ? ExoTheme.spacingMd : ExoTheme.spacingXl
                    Layout.alignment: Qt.AlignVCenter
                    Accessible.role: Accessible.StaticText
                    Accessible.name: "exosnap"
                }

                Flickable {
                    id: navStrip

                    objectName: "quickNavStrip"
                    Layout.fillWidth: true
                    Layout.maximumWidth: navRow.width
                    Layout.preferredWidth: navRow.width
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: root.titleBarHeight
                    contentWidth: navRow.width
                    contentHeight: height
                    flickableDirection: Flickable.HorizontalFlick
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true

                    function ensureVisible(tab: Item): void {
                        if (!tab)
                            return;
                        contentX = Math.max(0, Math.min(Math.max(0, contentWidth - width),
                            tab.x < contentX ? tab.x : Math.max(contentX, tab.x + tab.width - width)));
                    }

                    function revealCurrent(): void {
                        for (let i = 0; i < navRepeater.count; ++i) {
                            const tab = navRepeater.itemAt(i);
                            if (tab && (tab.activeFocus || root.currentPage === root.pageForTab(i))) {
                                ensureVisible(tab);
                                if (tab.activeFocus)
                                    return;
                            }
                        }
                    }

                    onWidthChanged: {
                        Qt.callLater(navStrip.revealCurrent);
                        Qt.callLater(titleBar.refreshChromeGeometry);
                    }
                    onContentWidthChanged: Qt.callLater(navStrip.revealCurrent)
                    onXChanged: Qt.callLater(titleBar.refreshChromeGeometry)

                    WheelHandler {
                        target: null
                        onWheel: event => {
                            const delta = event.pixelDelta.x || event.pixelDelta.y
                                          || event.angleDelta.x / 2 || event.angleDelta.y / 2;
                            navStrip.contentX = Math.max(0, Math.min(Math.max(0, navStrip.contentWidth - navStrip.width),
                                                                   navStrip.contentX - delta));
                            event.accepted = true;
                        }
                    }

                    Row {
                        id: navRow
                        height: navStrip.height
                        spacing: ExoTheme.spacingXs

                        Repeater {
                            id: navRepeater
                            objectName: "quickNavTabs"
                            model: root.navPages

                            delegate: ExoNavTab {
                                id: navTab
                                required property int index
                                required property string modelData

                                text: modelData
                                selected: root.currentPage === root.pageForTab(index)
                                compact: root.compactNav
                                enabled: root.navigationAllowed || selected
                                onClicked: root.navigateTo(root.pageForTab(index))
                                onSelectedChanged: {
                                    if (selected)
                                        Qt.callLater(navStrip.ensureVisible, navTab);
                                }
                                onActiveFocusChanged: {
                                    if (activeFocus)
                                        navStrip.ensureVisible(navTab);
                                }
                                Keys.onLeftPressed: {
                                    const previous = navRepeater.itemAt((index + navRepeater.count - 1) % navRepeater.count);
                                    if (previous)
                                        previous.forceActiveFocus(Qt.TabFocusReason);
                                }
                                Keys.onRightPressed: {
                                    const next = navRepeater.itemAt((index + 1) % navRepeater.count);
                                    if (next)
                                        next.forceActiveFocus(Qt.TabFocusReason);
                                }
                            }
                        }
                    }
                }
                // The drag handle. It has no visual and no input handler at all:
                // the band is dragged by Windows, because everything not listed
                // as interactive resolves to HTCAPTION.
                Item {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }

                // What the engine is doing, permanently visible, exactly as the
                // Widgets shell established. Deliberately NOT registered as an
                // interactive rect: it is a readout, so the band stays draggable
                // across it.
                SourceConfidence {
                    indicators: root.recordViewModel.confidenceIndicators
                    Layout.alignment: Qt.AlignVCenter
                }

                ExoStatusPill {
                    text: root.recordViewModel.stateText
                    tone: root.recordViewModel.stateTone
                    Layout.rightMargin: ExoTheme.spacingSm
                    Layout.alignment: Qt.AlignVCenter
                    // The readout can compress before destination labels lose space.
                    Layout.fillWidth: true
                    Layout.maximumWidth: implicitWidth
                    Layout.minimumWidth: 0
                    // The pill is elastic and its text changes with the recording
                    // state, so every state transition shifts the bell and all
                    // three window buttons sideways. Without this the pushed-down
                    // rects keep describing where those items used to be, and the
                    // Maximize button's HTMAXBUTTON rect in particular ends up
                    // beside the button: the band answers HTCAPTION where the
                    // button now is, so it drags instead of maximizing.
                    onWidthChanged: Qt.callLater(titleBar.refreshChromeGeometry)
                }

                NotificationBell {
                    id: notificationBell

                    notifications: root.notifications
                    Layout.alignment: Qt.AlignVCenter
                    Layout.minimumWidth: implicitWidth
                    onWidthChanged: Qt.callLater(titleBar.refreshChromeGeometry)
                }

                // The three window buttons declare a minimum equal to their own
                // size, so a band that overflows can never resolve it by clipping
                // Close off the right edge. It did exactly that at the 860 px
                // minimum window, which left the shipped shell with no visible
                // way to close it.
                WindowChromeButton {
                    id: minimizeButton

                    kind: "minimize"
                    Accessible.name: qsTr("Minimize")
                    Layout.leftMargin: ExoTheme.spacingSm
                    Layout.minimumWidth: implicitWidth
                    onClicked: root.minimizeRequested()
                }

                WindowChromeButton {
                    id: maximizeButton

                    kind: root.windowMaximized ? "restore" : "maximize"
                    Accessible.name: root.windowMaximized ? qsTr("Restore") : qsTr("Maximize")
                    Layout.minimumWidth: implicitWidth
                    // This button's rect answers HTMAXBUTTON, so Qt delivers no
                    // mouse event over it and `hovered` never becomes true. Only
                    // the pointer state is taken from the chrome here: activation
                    // arrives as QuickWindowChrome::maximizeButtonClicked, which
                    // the window itself already acts on. Handling it here as well
                    // toggles the window twice per click.
                    nonClientHovered: root.chrome ? root.chrome.maximizeButtonHovered : false
                    nonClientPressed: root.chrome ? root.chrome.maximizeButtonPressed : false
                    onClicked: root.maximizeRestoreRequested()
                }

                WindowChromeButton {
                    id: closeButton
                    objectName: "quickCloseButton"

                    kind: "close"
                    danger: true
                    Accessible.name: qsTr("Close")
                    Layout.minimumWidth: implicitWidth
                    onClicked: root.closeRequested()
                }
            }

            Rectangle {
                height: 1
                color: ExoTheme.line
                anchors {
                    right: parent.right
                    bottom: parent.bottom
                    left: parent.left
                }
            }
        }

        StackLayout {
            currentIndex: root.stackIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            RecordPage {
                recordViewModel: root.recordViewModel
                previewAdapter: root.previewAdapter
                shell: root.shell
                // displayedPage, not currentPage: `active` drives the live preview,
                // the webcam frame delivery and the meters, all of which are what
                // the page SHOWS. Following the request instead of the swap paused
                // the preview on the click, while the Record page was still the one
                // on screen waiting for an incubating destination -- a black preview
                // for as long as that first load took. The capture path does not
                // read this flag; while the engine owns the source the preview gate
                // stands still regardless of it.
                active: root.displayedPage === ShellAdapter.RecordPage
                benchmarkInteractionActive: root.benchmarkInteractionActive
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            Loader {
                id: editLoader
                asynchronous: true
                Layout.fillWidth: true
                Layout.fillHeight: true
                onStatusChanged: root.refreshDisplayedPage()
            }
            Loader {
                id: settingsLoader

                asynchronous: true
                Layout.fillWidth: true
                Layout.fillHeight: true
                onStatusChanged: root.refreshDisplayedPage()
            }

            Loader {
                id: diagnosticsLoader

                asynchronous: true
                Layout.fillWidth: true
                Layout.fillHeight: true
                onStatusChanged: root.refreshDisplayedPage()
            }

            Loader {
                id: aboutLoader

                asynchronous: true
                Layout.fillWidth: true
                Layout.fillHeight: true
                onStatusChanged: root.refreshDisplayedPage()
            }
        }
    }

    // The Diagnostics workspace's two navigation signals, forwarded from the
    // overview page through the workspace. setSource() carries property values
    // and not signal handlers, so they are connected here instead. The target is
    // null until the workspace is loaded, which is exactly when there is nothing
    // to connect to — the binding re-targets on load.
    //
    // ignoreUnknownSignals because the loaded item's type is deliberately not
    // known to this document any more: knowing it is what pulled DiagnosticsPage
    // into the startup compile.
    Connections {
        target: diagnosticsLoader.item
        ignoreUnknownSignals: true

        function onNavigateToLogsRequested(): void {
            root.navigateTo(ShellAdapter.LogsPage);
        }

        function onNavigateToSettingsRequested(): void {
            root.navigateTo(ShellAdapter.SettingsPage);
        }

        // The log view's own way back. Written straight to the section rather
        // than through navigateTo(): the destination does not change, so there
        // is no navigation for the guard to refuse.
        function onBackToOverviewRequested(): void {
            root.diagnosticsSection = ShellAdapter.DiagnosticsOverview;
        }

        // The workspace's OUTER loader is ready before its inner view is, and the
        // stack must not stop at the workspace and show the previous destination
        // until some unrelated loader happens to move. The inner readiness is part
        // of the destination's readiness, so it advances the displayed page too.
        function onOverviewReadyChanged(): void {
            root.refreshDisplayedPage();
        }

        function onLogsViewReadyChanged(): void {
            root.refreshDisplayedPage();
        }
    }

    // Built on the first time the bell is pressed. A Popup constructs its whole
    // contentItem with itself, so the hub's header, its empty state and — once
    // the model has rows — a delegate per notification existed from startup for a
    // surface most sessions never open. It lives out here rather than inside the
    // bell because a Loader IS an Item and would take part in the title band's
    // layout, which a Popup does not; the created hub still parents itself to the
    // bell, so its anchoring is unchanged.
    Loader {
        id: notificationHubLoader

        active: false

        sourceComponent: NotificationHub {
            parent: notificationBell
            notifications: root.notifications
        }
    }

    Connections {
        target: root.notifications

        function onHubOpenChanged(): void {
            // One-way: the hub stays resident after the first open, like the four
            // destinations. Its own `visible` binding takes over from here — it
            // is already true by the time this loads, so the first press opens it.
            if (root.notifications.hubOpen)
                notificationHubLoader.active = true;
        }
    }

    Component.onCompleted: {
        if (root.editSession.durationMs > 0)
            root.currentPage = ShellAdapter.EditPage;
        root.loadDestination(root.currentPage);
        root.refreshDisplayedPage();
    }

    Connections {
        target: root.editSession
        function onEditPageRequested(): void { root.navigateTo(ShellAdapter.EditPage); }
    }

    Binding {
        target: root.editPlayer
        property: "surfaceVisible"
        value: root.editPageVisible
    }
    Loader {
        id: whatsNewLoader

        // Where the keyboard was before the card took it, read off the card while
        // it still exists. The Loader outlives it, which is why the restore
        // happens here: a focus assignment made from the dying item's own
        // destruction handler is undone by this focus scope coming down with it.
        property Item focusReturn: null
        property bool sourceLoaded: false

        anchors.fill: parent
        active: root.whatsNew.active
        z: 2

        onLoaded: {
            const card = whatsNewLoader.item as WhatsNewOverlay;
            whatsNewLoader.focusReturn = card !== null ? card.focusReturnItem : null;
        }

        onActiveChanged: {
            if (whatsNewLoader.active) {
                if (!whatsNewLoader.sourceLoaded) {
                    whatsNewLoader.sourceLoaded = true;
                    whatsNewLoader.setSource(Qt.resolvedUrl("WhatsNewOverlay.qml"), {
                        whatsNew: root.whatsNew,
                        // The scrim covers the band; the card stays below it, like
                        // every other in-window surface.
                        contentTopInset: root.titleBarHeight,
                        focus: true
                    });
                }
                return;
            }
            const target = whatsNewLoader.focusReturn;
            whatsNewLoader.focusReturn = null;
            // A closed overlay must not leave the window without a focus owner:
            // Tab from nowhere goes nowhere, and where the user was is the control
            // they opened this from. A null target is the post-update auto-show,
            // raised before anything could be focused; the window's root item owns
            // the chain in that case and Tab still walks the page, so there is
            // nothing to restore and nothing to invent.
            if (target !== null && target.enabled && target.visible)
                target.forceActiveFocus(Qt.OtherFocusReason);
        }
    }

    // Above the editor: recovery is a startup decision about a PREVIOUS session,
    // so it must not end up behind a surface opened for the current one.
    Loader {
        id: recoveryOverlayLoader

        // Where the keyboard was before this surface took it. The card publishes
        // it; restoring it is this loader's job, because the card is gone by the
        // time there is anything to restore. Added late: the contract landed with
        // the What's-new overlay, after these three were already written, so they
        // closed and left the window with no focus owner at all.
        property Item focusReturn: null
        property bool sourceLoaded: false

        anchors.fill: parent
        active: root.recovery.surfaceOpen
        z: 2

        onLoaded: {
            const card = recoveryOverlayLoader.item as ExoOverlayCard;
            recoveryOverlayLoader.focusReturn = card !== null ? card.focusReturnItem : null;
        }

        onActiveChanged: {
            if (recoveryOverlayLoader.active) {
                if (!recoveryOverlayLoader.sourceLoaded) {
                    recoveryOverlayLoader.sourceLoaded = true;
                    recoveryOverlayLoader.setSource(Qt.resolvedUrl("RecoveryOverlay.qml"), {
                        recovery: root.recovery,
                        // The scrim covers the shell including its title band --
                        // the window behind a modal must not read as still usable
                        // -- but the card stays below it, or at the 860x700
                        // minimum window its top edge lands on the brand, the
                        // navigation and the window buttons.
                        contentTopInset: root.titleBarHeight,
                        focus: true
                    });
                }
                return;
            }
            const target = recoveryOverlayLoader.focusReturn;
            recoveryOverlayLoader.focusReturn = null;
            if (target !== null && target.enabled && target.visible)
                target.forceActiveFocus(Qt.OtherFocusReason);
        }
    }

    // Topmost: a failed recording is the most recent thing the user did, and it
    // is the one surface that must never be hidden behind another.
    Loader {
        id: recordingErrorLoader

        // Where the keyboard was before this surface took it. The card publishes
        // it; restoring it is this loader's job, because the card is gone by the
        // time there is anything to restore. Added late: the contract landed with
        // the What's-new overlay, after these three were already written, so they
        // closed and left the window with no focus owner at all.
        property Item focusReturn: null
        property bool sourceLoaded: false

        anchors.fill: parent
        active: root.recordingError.active
        z: 3

        onLoaded: {
            const card = recordingErrorLoader.item as ExoOverlayCard;
            recordingErrorLoader.focusReturn = card !== null ? card.focusReturnItem : null;
        }

        onActiveChanged: {
            if (recordingErrorLoader.active) {
                if (!recordingErrorLoader.sourceLoaded) {
                    recordingErrorLoader.sourceLoaded = true;
                    recordingErrorLoader.setSource(Qt.resolvedUrl("RecordingErrorOverlay.qml"), {
                        error: root.recordingError,
                        contentTopInset: root.titleBarHeight,
                        focus: true
                    });
                }
                return;
            }
            const target = recordingErrorLoader.focusReturn;
            recordingErrorLoader.focusReturn = null;
            if (target !== null && target.enabled && target.visible)
                target.forceActiveFocus(Qt.OtherFocusReason);
        }
    }

    // Startup consent surface. Above the editor for the same reason recovery is,
    // and below the recording error: a failure the user has just caused outranks
    // a report about a session that ended before this one began.
    Loader {
        id: crashReportLoader

        // Where the keyboard was before this surface took it. The card publishes
        // it; restoring it is this loader's job, because the card is gone by the
        // time there is anything to restore. Added late: the contract landed with
        // the What's-new overlay, after these three were already written, so they
        // closed and left the window with no focus owner at all.
        property Item focusReturn: null
        property bool sourceLoaded: false

        anchors.fill: parent
        active: root.crashReport.active
        z: 2

        onLoaded: {
            const card = crashReportLoader.item as ExoOverlayCard;
            crashReportLoader.focusReturn = card !== null ? card.focusReturnItem : null;
        }

        onActiveChanged: {
            if (crashReportLoader.active) {
                if (!crashReportLoader.sourceLoaded) {
                    crashReportLoader.sourceLoaded = true;
                    crashReportLoader.setSource(Qt.resolvedUrl("CrashReportOverlay.qml"), {
                        crash: root.crashReport,
                        contentTopInset: root.titleBarHeight,
                        focus: true
                    });
                }
                return;
            }
            const target = crashReportLoader.focusReturn;
            crashReportLoader.focusReturn = null;
            if (target !== null && target.enabled && target.visible)
                target.forceActiveFocus(Qt.OtherFocusReason);
        }
    }
}
