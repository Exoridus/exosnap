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

    // Nav index -> stack index. Every canonical destination now has a page, so the
    // two indices coincide; the mapping stays explicit because the nav order is a
    // product decision (Record, Device, Settings, Diagnostics, Logs, About).
    readonly property int stackIndex: root.currentPage

    objectName: "quickAppShell"

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

                Label {
                    text: qsTr("ExoSnap")
                    textFormat: Text.PlainText
                    color: ExoTheme.text
                    Layout.rightMargin: ExoTheme.spacingXl
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 15
                        weight: Font.DemiBold
                    }
                }

                Repeater {
                    id: navRepeater

                    // Nav order is a product decision (Record, Device, Settings,
                    // Diagnostics, Logs, About). Kept as a list rather than six
                    // copies of the same button so the hit-test rects can be
                    // collected by index.
                    model: [qsTr("Record"), qsTr("Device"), qsTr("Settings"),
                            qsTr("Diagnostics"), qsTr("Logs"), qsTr("About")]

                    delegate: ExoButton {
                        required property int index
                        required property string modelData

                        text: modelData
                        quiet: true
                        selectable: true
                        selected: root.currentPage === index
                        onClicked: root.currentPage = index
                        onWidthChanged: Qt.callLater(titleBar.refreshChromeGeometry)
                    }
                }

                // The drag handle. It has no visual and no input handler at all:
                // the band is dragged by Windows, because everything not listed
                // as interactive resolves to HTCAPTION.
                Item {
                    Layout.fillWidth: true
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
                }

                NotificationBell {
                    id: notificationBell

                    notifications: root.notifications
                    onWidthChanged: Qt.callLater(titleBar.refreshChromeGeometry)

                    NotificationHub {
                        parent: notificationBell
                        notifications: root.notifications
                    }
                }

                WindowChromeButton {
                    id: minimizeButton

                    kind: "minimize"
                    Accessible.name: qsTr("Minimize")
                    Layout.leftMargin: ExoTheme.spacingSm
                    onClicked: root.minimizeRequested()
                }

                WindowChromeButton {
                    id: maximizeButton

                    kind: root.windowMaximized ? "restore" : "maximize"
                    Accessible.name: root.windowMaximized ? qsTr("Restore") : qsTr("Maximize")
                    onClicked: root.maximizeRestoreRequested()
                }

                WindowChromeButton {
                    id: closeButton

                    kind: "close"
                    danger: true
                    Accessible.name: qsTr("Close")
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
                active: root.currentPage === 0
                benchmarkInteractionActive: root.benchmarkInteractionActive
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            DevicePage {
                device: root.deviceAdapter
                Layout.fillWidth: true
                Layout.fillHeight: true
                onSettingsRequested: root.currentPage = 2
            }

            SettingsPage {
                settings: root.settingsAdapter
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            DiagnosticsPage {
                diagnostics: root.diagnosticsAdapter
                Layout.fillWidth: true
                Layout.fillHeight: true
                onNavigateToLogsRequested: root.currentPage = 4
                onNavigateToDeviceRequested: root.currentPage = 1
                onNavigateToSettingsRequested: root.currentPage = 2
            }

            LogsPage {
                logs: root.logsAdapter
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            AboutPage {
                aboutViewModel: root.aboutViewModel
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }

    // Handing the surface a clip IS the request to show it: setEditContext is
    // only ever called when a recording has finished and the user asked to edit
    // it. Dismissal is the session's own closeRequested.
    // A context handed over before the scene existed (the visual harness seeds one
    // during load) has already fired its signal by the time the Connections below
    // is live, so the initial state is read directly.
    Component.onCompleted: root.editOverlayOpen = root.editSession.durationMs > 0

    Connections {
        target: root.editSession

        function onClipChanged(): void {
            root.editOverlayOpen = root.editSession.durationMs > 0;
        }

        function onCloseRequested(): void {
            root.editOverlayOpen = false;
        }
    }

    Loader {
        id: editOverlayLoader

        anchors.fill: parent
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
            focus: true
        }
    }
}
