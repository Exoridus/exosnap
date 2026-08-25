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
    // Edit/Output/Save is an overlay over the Record page (ADR 0022), never a
    // nav destination — so its visibility is shell state, not a stack index.
    property bool editOverlayOpen: false

    // QCR-001. An open edit session is STATE OF THE RECORD DESTINATION, not a
    // modality of the application: it survives a page change untouched and is
    // simply not on screen while another destination is. Loaded is the session;
    // visible is where that session is shown.
    readonly property bool editOverlayVisible: root.editOverlayOpen && root.currentPage === ShellAdapter.RecordPage

    // Page index -> stack index. One index space, and it IS ShellAdapter::Page:
    // the StackLayout's child order below is the enum's order, so no separate
    // mapping exists to drift. QCR-716 replaced the bare 0..4 literals that used
    // to spell it out here with the enumerators themselves.
    readonly property int stackIndex: root.currentPage

    // Loads the destination being navigated to. Written as a switch over the same
    // index space rather than as a generated list: the five destinations are a
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
    // URL is a string until it is loaded, so the four page documents leave the
    // startup compile entirely — DiagnosticsPage 34,7 ms, LogsPage 17,9 ms,
    // SettingsPage 8,8 ms, AboutPage 0,5 ms of it (QCR-615). The trade is
    // deliberate: the first deliberate visit to a page pays that page's compile.
    //
    // The properties below are the pages' required adapters. Every one of them is
    // a required property of this shell, handed in once by Main and never
    // reassigned, so an initial value is the whole contract — there is no binding
    // to lose. Signals are the exception: setSource carries values, not handlers,
    // so DiagnosticsPage's two navigation signals are connected separately below.
    //
    // Idempotent by status: a second navigation to the same page finds it loaded
    // and does nothing, which is the resident-page contract QCR-602 established.
    function loadDestination(page: int): void {
        switch (page) {
        case ShellAdapter.SettingsPage:
            if (settingsLoader.status === Loader.Null)
                settingsLoader.setSource(Qt.resolvedUrl("SettingsPage.qml"), {
                    settings: root.settingsAdapter
                });
            break;
        case ShellAdapter.DiagnosticsPage:
            if (diagnosticsLoader.status === Loader.Null)
                diagnosticsLoader.setSource(Qt.resolvedUrl("DiagnosticsPage.qml"), {
                    diagnostics: root.diagnosticsAdapter,
                    device: root.deviceAdapter
                });
            break;
        case ShellAdapter.LogsPage:
            if (logsLoader.status === Loader.Null)
                logsLoader.setSource(Qt.resolvedUrl("LogsPage.qml"), {
                    logs: root.logsAdapter
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

    onCurrentPageChanged: root.loadDestination(root.currentPage)

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
        property: "editSurfaceVisible"
        value: root.editOverlayVisible
    }

    // ── The one navigation edge (QCR-001) ────────────────────────────────────
    //
    // Every navigation intent in the product writes the destination HERE, and
    // nowhere else: the five tab delegates, Ctrl+1..5, the Diagnostics page's
    // two jump signals, and ShellAdapter::navigateToPageRequested — which is
    // itself emitted from six production paths (the OpenUpdate / ChangeFolder /
    // OpenHotkeys / OpenDiagnostics notification actions, the Diagnostics
    // blocker jump, the recovery Continue, the recording-error log jump).
    //
    // Before this the policy sat on two AFFORDANCES instead — `enabled` on the
    // tab delegate and on the shortcuts — while the edge wrote `currentPage`
    // unconditionally from Main.qml. So a notification toast could already swap
    // the page underneath an open Edit workspace, which is precisely what those
    // two disabled affordances claimed to prevent. One policy needs one edge.
    function navigateTo(page: int): void {
        if (!root.navigationAllowed)
            return;
        root.currentPage = page;
    }

    // The whole policy, in one expression.
    //
    // An open edit session is deliberately NOT in it (QCR-001): navigating away
    // from Record does not close it, does not ask about unsaved trim points and
    // does not end the clip — it only stops showing it. Three of the four
    // surfaces that ARE in it are modal about a question the user has not
    // answered yet, and a page swapped behind one of them is a page the user
    // never asked for.
    //
    // "What's new" is the fourth for a different reason: it asks nothing, but its
    // scrim covers the title band, so the POINTER route to the tabs is already
    // refused. Leaving Ctrl+1..5 live would make the keyboard disagree with the
    // affordance — exactly the split QCR-001 was about, in the other direction.
    //
    // The close guard is the fifth, and the only one whose surface is a Dialog
    // rather than an in-shell overlay: its scrim does not cover the desktop
    // toast, which is its own always-on-top window and reaches navigateTo()
    // directly. Without this term a toast action swaps the page underneath an
    // unanswered close prompt.
    readonly property bool navigationAllowed: !root.recovery.surfaceOpen && !root.recordingError.active
                                              && !root.crashReport.active && !root.whatsNew.active
                                              && !root.shell.closeGuardActive

    // Every destination, directly. Five words fit the band at the 860 px minimum
    // window, so hiding three of them behind a glyph bought nothing and cost a
    // click plus a menu on the way to Diagnostics — the page a user goes to
    // precisely when something is already wrong.
    readonly property var navPages: [qsTr("Record"), qsTr("Settings"), qsTr("Diagnostics"), qsTr("Logs"), qsTr("About")]

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
        onActivated: root.navigateTo(ShellAdapter.SettingsPage)
    }

    Shortcut {
        sequence: "Ctrl+3"
        context: Qt.WindowShortcut
        enabled: root.navigationAllowed
        onActivated: root.navigateTo(ShellAdapter.DiagnosticsPage)
    }

    Shortcut {
        sequence: "Ctrl+4"
        context: Qt.WindowShortcut
        enabled: root.navigationAllowed
        onActivated: root.navigateTo(ShellAdapter.LogsPage)
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

            // The band's own rects, in shell coordinates. Recomputed whenever
            // anything that can move them changes — the width (every resize),
            // the selected page (the selected tab is bolder and therefore
            // wider), and the bell's unread dot.
            function refreshChromeGeometry(): void {
                if (!root.chrome)
                    return;

                root.chrome.clearInteractiveRects();
                for (let i = 0; i < navRepeater.count; ++i) {
                    const tab = navRepeater.itemAt(i);
                    if (tab)
                        root.chrome.addInteractiveRect(rectInShell(tab));
                }
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

                Repeater {
                    id: navRepeater

                    // Named so the navigation-lifecycle test can reach the five
                    // delegates through itemAt() and assert the AFFORDANCE, not
                    // only the edge behind it: QCR-001 was a regression in the
                    // delegate's `enabled` binding, and an assertion that only
                    // calls navigateTo() would not have seen it. The delegates
                    // themselves are not QObject children of the window, so the
                    // repeater is the way in.
                    objectName: "quickNavTabs"

                    // Nav order is a product decision. Kept as a list rather than
                    // copies of the same button so the hit-test rects can be
                    // collected by index.
                    model: root.navPages

                    delegate: ExoNavTab {
                        required property int index
                        required property string modelData

                        text: modelData
                        selected: root.currentPage === index
                        compact: root.compactNav
                        // An open Edit workspace is deliberately absent from this
                        // (QCR-001): it is state of the Record destination, so
                        // leaving Record hides it and returning shows the same
                        // session again. It never swaps a page UNDER a covering
                        // workspace, because the workspace is only ever visible
                        // on Record — see `editOverlayVisible`.
                        enabled: root.navigationAllowed
                        Layout.alignment: Qt.AlignVCenter
                        // Shrinkable to nothing on purpose. Everything to the
                        // right of the drag handle is fixed-size, so when the
                        // band runs out of room the tabs are the only things
                        // that may give — never the close button.
                        //
                        // QCR-511. `minimumWidth: 0` alone did NOT achieve that:
                        // a layout item with `fillWidth` false is FIXED at its
                        // preferred size (Qt Quick Layouts, Layout attached
                        // properties), so the minimum was never consulted. Once
                        // the drag handle — the band's only fillWidth item —
                        // reached zero, the row simply laid the rest out past
                        // its own right edge, and what fell off the end was the
                        // status pill, the bell and all three window buttons.
                        // Measured at the 860 px minimum window with a +40 %
                        // text expansion: the window had no visible way to be
                        // closed, minimised or moved. `fillWidth` with the
                        // implicit width as a CEILING makes the tab shrinkable
                        // without letting it grow past its label, so nothing
                        // changes at any width where the band already fits.
                        Layout.fillWidth: true
                        Layout.maximumWidth: implicitWidth
                        Layout.minimumWidth: 0
                        onClicked: root.navigateTo(index)
                        onWidthChanged: Qt.callLater(titleBar.refreshChromeGeometry)
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
                ExoStatusPill {
                    text: root.recordViewModel.stateText
                    tone: root.recordViewModel.stateTone
                    Layout.rightMargin: ExoTheme.spacingSm
                    Layout.alignment: Qt.AlignVCenter
                    // Never wider than its own text, and allowed to be narrower.
                    // It used to declare its implicit width as a MINIMUM, which
                    // made a long state string incompressible and left the
                    // navigation tabs — the only other shrinkable thing in the
                    // band — to pay for it. A readout may elide; a destination
                    // may not disappear.
                    //
                    // `fillWidth` for the same reason the tabs now carry it: the
                    // maximum and minimum above were inert without it.
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

        // ── The five destinations ────────────────────────────────────────────
        //
        // Record is eager; the other four are compiled and built on their first
        // visit and then stay resident.
        //
        // Before this, all five were direct children of the StackLayout, so a
        // launch that never left Record still compiled and instantiated
        // Settings, Diagnostics, Logs and About: 8 700 `Creating` events and all
        // 134 `Compiling` events happened before the first frame, among them 154
        // ExoSettingRow and the 64 ComboBox popups of a page the user had not
        // opened. The cost grew with every settings row added.
        //
        // Resident after the first visit rather than unloaded on leave: page
        // state that is not in an adapter (scroll position, an open disclosure,
        // a Settings draft) is the user's place in the page, and a stack whose
        // pages forget where you were is worse than a slower first visit. Memory
        // is not the constraint here — the eager version held all five for the
        // whole session and nobody measured a problem.
        //
        // The five direct tabs are untouched: this changes WHEN a destination's
        // content is built, never how many destinations there are.
        StackLayout {
            currentIndex: root.stackIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            RecordPage {
                recordViewModel: root.recordViewModel
                previewAdapter: root.previewAdapter
                shell: root.shell
                active: root.currentPage === ShellAdapter.RecordPage
                benchmarkInteractionActive: root.benchmarkInteractionActive
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            // The four loaders carry no source of their own: loadDestination()
            // sets it, with the page's required adapters as initial properties.
            // They stay active so that the assignment loads immediately — an
            // inactive Loader would defer the load to whenever it is activated,
            // which is one more state for the same moment to be in.
            Loader {
                id: settingsLoader

                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            Loader {
                id: diagnosticsLoader

                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            Loader {
                id: logsLoader

                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            Loader {
                id: aboutLoader

                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }

    // Diagnostics' two navigation signals. They used to be inline handlers on the
    // loader's sourceComponent; setSource() carries property values and not signal
    // handlers, so they are connected here instead. The target is null until the
    // page is loaded, which is exactly when there is nothing to connect to — the
    // binding re-targets on load.
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

    // Handing the surface a clip IS the request to show it: setEditContext is
    // only ever called when a recording has finished and the user asked to edit
    // it. Dismissal is the session's own closeRequested.
    // A context handed over before the scene existed (the visual harness seeds one
    // during load) has already fired its signal by the time the Connections below
    // is live, so the initial state is read directly.
    Component.onCompleted: {
        root.editOverlayOpen = root.editSession.durationMs > 0;
        root.loadDestination(root.currentPage);
    }

    Connections {
        target: root.editSession

        function onClipChanged(): void {
            root.editOverlayOpen = root.editSession.durationMs > 0;
        }

        function onCloseRequested(): void {
            root.editOverlayOpen = false;
        }
    }

    // The Edit surface is still a LAYER rather than a stack destination (ADR
    // 0022 is untouched: nothing about who owns the clip, the decoder or the
    // export changed) — but it now occupies the same content region every page
    // does, below the shell's own 40 px band, instead of the whole window.
    //
    // Covering the window was the source of the "giant modal" reading, and of
    // one outright defect: the shell's minimize, maximize and close buttons sat
    // UNDER the editor, so a window with the editor open could not be closed or
    // dragged by its own chrome. Anchoring below the title bar puts the brand,
    // the navigation, the status pill, the notification bell and the three
    // window buttons back where they always are.
    //
    // This and the three loaders after it keep an id no expression reads (QCR-706
    // removed eleven such ids elsewhere). They are kept deliberately: the four are
    // the shell's overlay LAYER STACK, their declaration order IS their z-order,
    // and the comments above each one argue that order by name. An anonymous
    // Loader would leave those arguments pointing at nothing.
    //
    // setSource(url, properties) rather than an inline sourceComponent, for the
    // same reason loadDestination() above uses it for the four nav pages
    // (QCR-615): an inline Component is part of THIS document, so the engine
    // compiles EditOverlay's whole type -- and everything it pulls in -- before
    // the first frame, even though the Loader stays inactive until a clip
    // exists. Measured on this tree: EditOverlay.qml + EditExportPanel.qml alone
    // cost ~385 ms of compile time on a launch that never opens the editor.
    // sourceLoaded guards the one-time setSource call; `source` itself is
    // sticky across active going false then true again, so a later reopen does
    // not recompile or re-snapshot the initial properties.
    Loader {
        id: editOverlayLoader

        property bool sourceLoaded: false

        anchors {
            fill: parent
            topMargin: root.titleBarHeight
        }
        // Two separate facts, and QCR-001 turns on keeping them apart.
        //
        // ACTIVE follows the SESSION: the surface is built when a clip is handed
        // over and unloaded when the session is closed — never merely because
        // the user looked at Settings. Unloading on a page change would destroy
        // the scene-graph video item and every piece of state that is not in an
        // adapter (the rail's scroll position, the timeline zoom, the focus),
        // which is the same argument QCR-602 used to keep the four destinations
        // resident after their first visit.
        //
        // VISIBLE follows the DESTINATION: the workspace belongs to Record, so
        // that is the only page it is on screen for. This is what makes an
        // unlocked tab safe — the page never changes underneath a covering
        // surface, because the surface goes with the page. An invisible item
        // also takes no input and receives no key events, so the editor's
        // surface-local keys cannot fire while Settings is on screen.
        active: root.editOverlayOpen
        visible: root.editOverlayVisible
        z: 1

        // Coming back to Record returns the keyboard to the workspace. Without
        // it the editor is on screen but deaf: Escape, and every surface-local
        // key below it, would go to whatever held the focus on the page the user
        // just left. Called on load as well, so the first open is no different
        // from a return.
        onVisibleChanged: root.focusEditWorkspace()
        onLoaded: root.focusEditWorkspace()
        onActiveChanged: {
            if (!editOverlayLoader.active || editOverlayLoader.sourceLoaded)
                return;
            editOverlayLoader.sourceLoaded = true;
            editOverlayLoader.setSource(Qt.resolvedUrl("EditOverlay.qml"), {
                session: root.editSession,
                timeline: root.editTimeline,
                player: root.editPlayer,
                exporter: root.editExport,
                focus: true
            });
        }
    }

    function focusEditWorkspace(): void {
        if (!editOverlayLoader.visible)
            return;
        // `Loader.item` is typed QObject, so the cast is what tells qmllint (and
        // the compiler) that this is the Item whose focus is being taken.
        const workspace = editOverlayLoader.item as Item;
        if (workspace !== null)
            workspace.forceActiveFocus();
    }

    // Leaving Record pauses the preview; it does not end the session and does
    // not seek. Playing video and audio out of a surface the user cannot see is
    // both surprising on Settings and decoder work nobody asked for. Returning
    // leaves it paused at the same position — starting playback again is the
    // user's own action.
    //
    // Same shape as Main.qml's `previewAdapter.surfaceVisible` binding, and for
    // the same reason: the decision is C++'s (EditPlayerAdapter), the fact is
    // the shell's.
    Binding {
        target: root.editPlayer
        property: "surfaceVisible"
        value: root.editOverlayVisible
    }

    // Release notes (product-spec, "What's new (shipped)"). Above the editor for
    // the same reason recovery is, and FIRST among the equal-z surfaces below, so
    // that if one of them were ever raised while this is up it draws over the
    // changelog rather than under it. The composition root already keeps that
    // from happening — the post-update auto-show waits for the blocking surfaces
    // to clear — so this is the ordering as a fallback, not as the policy.
    // setSource(url, properties) rather than an inline sourceComponent -- see
    // editOverlayLoader above. Same trade for the other three overlays below.
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
