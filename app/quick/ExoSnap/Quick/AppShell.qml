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
    required property NotificationsAdapter notifications
    required property RecoveryAdapter recovery
    required property RecordingErrorAdapter recordingError
    required property CrashReportAdapter crashReport
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
    property int currentPage: 0
    // Edit/Output/Save is an overlay over the Record page (ADR 0022), never a
    // nav destination — so its visibility is shell state, not a stack index.
    property bool editOverlayOpen: false

    // Page index -> stack index. Single index space, mirroring ShellAdapter::Page:
    // Record 0, Settings 1, Diagnostics 2, Logs 3, About 4.
    readonly property int stackIndex: root.currentPage

    // Activates the Loader behind the destination being navigated to. Written as
    // a switch over the same index space rather than as a generated list: the
    // five destinations are a product decision, not a collection.
    //
    // Called from onCurrentPageChanged AND from Component.onCompleted, because a
    // shell constructed with a non-zero currentPage (the --visual-page harness
    // sets it after load, but nothing guarantees that ordering) would otherwise
    // show an empty stack page.
    function loadDestination(page: int): void {
        switch (page) {
        case 1:
            settingsLoader.active = true;
            break;
        case 2:
            diagnosticsLoader.active = true;
            break;
        case 3:
            logsLoader.active = true;
            break;
        case 4:
            aboutLoader.active = true;
            break;
        default:
            break;
        }
    }

    onCurrentPageChanged: root.loadDestination(root.currentPage)

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
    // Disabled rather than merely ineffective while a covering surface is up:
    // the Edit workspace deliberately disables the nav tabs (QCR-001), and a
    // shortcut that did what the disabled control cannot would be the same
    // conditional navigation that decision ruled out. The three blocking
    // surfaces are modal about a session the user has not answered for yet.
    readonly property bool navigationShortcutsEnabled: !root.editOverlayOpen
                                                       && !root.recovery.surfaceOpen
                                                       && !root.recordingError.active
                                                       && !root.crashReport.active

    // Written out rather than generated from `navPages`: five destinations is a
    // product decision (CLAUDE.md), not a list length, and a Repeater of
    // Shortcuts would need a delegate item that exists for nothing else.
    Shortcut {
        sequence: "Ctrl+1"
        context: Qt.WindowShortcut
        enabled: root.navigationShortcutsEnabled
        onActivated: root.currentPage = 0
    }

    Shortcut {
        sequence: "Ctrl+2"
        context: Qt.WindowShortcut
        enabled: root.navigationShortcutsEnabled
        onActivated: root.currentPage = 1
    }

    Shortcut {
        sequence: "Ctrl+3"
        context: Qt.WindowShortcut
        enabled: root.navigationShortcutsEnabled
        onActivated: root.currentPage = 2
    }

    Shortcut {
        sequence: "Ctrl+4"
        context: Qt.WindowShortcut
        enabled: root.navigationShortcutsEnabled
        onActivated: root.currentPage = 3
    }

    Shortcut {
        sequence: "Ctrl+5"
        context: Qt.WindowShortcut
        enabled: root.navigationShortcutsEnabled
        onActivated: root.currentPage = 4
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
                ExoBrandMark {
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    Layout.alignment: Qt.AlignVCenter
                }

                Row {
                    Layout.leftMargin: ExoTheme.spacingSm - ExoTheme.spacingXs
                    // The one gap in the band that separates identity from
                    // navigation, so it is the first thing to give when five
                    // destinations have to fit beside three window buttons.
                    Layout.rightMargin: root.compactNav ? ExoTheme.spacingMd : ExoTheme.spacingXl
                    Layout.alignment: Qt.AlignVCenter

                    // NOT translatable, and now said so. A product name is the
                    // one string in a UI that must read identically in every
                    // language, and marking it `qsTr` also made the
                    // text-expansion harness (QCR-511) grow the wordmark — which
                    // measured 80 px of pressure on the navigation that no real
                    // translation will ever apply.
                    Label {
                        text: "exo"
                        textFormat: Text.PlainText
                        color: ExoTheme.text
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontBrand
                            weight: Font.DemiBold
                        }
                    }

                    Label {
                        text: "snap"
                        textFormat: Text.PlainText
                        color: ExoTheme.accent
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontBrand
                            weight: Font.DemiBold
                        }
                    }
                }

                Repeater {
                    id: navRepeater

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
                        // The Edit workspace occupies the content area below
                        // this band, with Record as its parent context. Leaving
                        // the tabs live would let a click swap the page UNDER a
                        // workspace that still covers it — and routing the click
                        // through the editor's unsaved-edits guard would make
                        // navigation conditional, which it is not anywhere else
                        // in the product. The same lock the transport's source
                        // controls take during a recording, for the same reason:
                        // Back is the way out.
                        enabled: !root.editOverlayOpen
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
                        onClicked: root.currentPage = index
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
        // Record is eager; the other four are built on their first visit and
        // then stay resident.
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
                active: root.currentPage === 0
                benchmarkInteractionActive: root.benchmarkInteractionActive
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            Loader {
                id: settingsLoader

                active: false
                Layout.fillWidth: true
                Layout.fillHeight: true

                sourceComponent: SettingsPage {
                    settings: root.settingsAdapter
                }
            }

            Loader {
                id: diagnosticsLoader

                active: false
                Layout.fillWidth: true
                Layout.fillHeight: true

                sourceComponent: DiagnosticsPage {
                    diagnostics: root.diagnosticsAdapter
                    device: root.deviceAdapter
                    onNavigateToLogsRequested: root.currentPage = 3
                    onNavigateToSettingsRequested: root.currentPage = 1
                }
            }

            Loader {
                id: logsLoader

                active: false
                Layout.fillWidth: true
                Layout.fillHeight: true

                sourceComponent: LogsPage {
                    logs: root.logsAdapter
                }
            }

            Loader {
                id: aboutLoader

                active: false
                Layout.fillWidth: true
                Layout.fillHeight: true

                sourceComponent: AboutPage {
                    aboutViewModel: root.aboutViewModel
                }
            }
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
    Loader {
        id: editOverlayLoader

        anchors {
            fill: parent
            topMargin: root.titleBarHeight
        }
        // Unloaded when dismissed: the overlay owns a scene-graph video item and
        // a decoder session, neither of which should sit behind a hidden page.
        active: root.editOverlayOpen
        z: 1

        sourceComponent: EditOverlay {
            session: root.editSession
            timeline: root.editTimeline
            player: root.editPlayer
            exporter: root.editExport
            focus: true
        }
    }

    // Above the editor: recovery is a startup decision about a PREVIOUS session,
    // so it must not end up behind a surface opened for the current one.
    Loader {
        id: recoveryOverlayLoader

        anchors.fill: parent
        active: root.recovery.surfaceOpen
        z: 2

        sourceComponent: RecoveryOverlay {
            recovery: root.recovery
            // The scrim covers the shell including its title band — the window
            // behind a modal must not read as still usable — but the card stays
            // below it, or at the 860x700 minimum window its top edge lands on
            // the brand, the navigation and the window buttons.
            contentTopInset: root.titleBarHeight
            focus: true
        }
    }

    // Topmost: a failed recording is the most recent thing the user did, and it
    // is the one surface that must never be hidden behind another.
    Loader {
        id: recordingErrorLoader

        anchors.fill: parent
        active: root.recordingError.active
        z: 3

        sourceComponent: RecordingErrorOverlay {
            error: root.recordingError
            contentTopInset: root.titleBarHeight
            focus: true
        }
    }

    // Startup consent surface. Above the editor for the same reason recovery is,
    // and below the recording error: a failure the user has just caused outranks
    // a report about a session that ended before this one began.
    Loader {
        id: crashReportLoader

        anchors.fill: parent
        active: root.crashReport.active
        z: 2

        sourceComponent: CrashReportOverlay {
            crash: root.crashReport
            contentTopInset: root.titleBarHeight
            focus: true
        }
    }
}
