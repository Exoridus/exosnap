import QtQuick
import QtQuick.Controls

ApplicationWindow {
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
    required property OverlayAdapter overlays
    required property ShellPresenceAdapter shellPresence
    required property TrayAdapter trayAdapter
    // Set once every close guard has cleared, so the re-issued close() is not
    // caught by the same guards again.
    property bool closeApproved: false
    property bool benchmarkInteractionActive: false
    property bool noActivate: false
    // ADR 0033. The destination the pre-elevation instance was showing, handed
    // back by the relaunch. Applied as the shell's STARTING page, not as a
    // navigation: the window is still hidden at this point and nothing has
    // happened yet that a navigation policy could have an opinion about.
    //
    // It used to arrive as a navigateToPageRequested() emitted straight after
    // the engine loaded. That moment is not neutral — a recovery surface or a
    // crash prompt raised during startup is already up by then, so the one
    // navigation edge would refuse the restore for a reason that has nothing to
    // do with it.
    property int landingPage: ShellAdapter.RecordPage

    // Resolved in C++ (QuickWindowGeometry) from the persisted geometry, clamped
    // onto a connected screen's work area. Supplied as an initial property so the
    // window carries its final placement from the start. The maximized state is
    // applied separately, after load, because a maximized window still needs this
    // rect as its restore rect.
    required property rect initialGeometry
    // Single source of truth with the clamp: ui::theme::ExoSnapMetrics.
    required property size minimumWindowSize

    x: root.initialGeometry.x
    y: root.initialGeometry.y
    width: root.initialGeometry.width
    height: root.initialGeometry.height
    minimumWidth: root.minimumWindowSize.width
    minimumHeight: root.minimumWindowSize.height
    // C++ OWNS THE FIRST SHOW (QuickApplication::load). Deliberately false, and
    // it must stay false.
    //
    // `visible: true` here shows the window part-way through engine load, at a
    // point where Qt has already created the HWND but has NOT yet applied the
    // FramelessWindowHint below. While the window still carries a native frame
    // Qt offsets every x/y/width it is given by that frame, so the rect the user
    // saw first was the intended one inflated by a frame the window does not
    // keep -- measured: 400,120 1280x820 came up as 392,89 1296x820, corrected a
    // frame later. Nothing declarative here can fix that ordering, because the
    // ordering is Qt's.
    //
    // So the window is built hidden and C++ shows it once the flags, the native
    // style and the geometry are all final. Harnesses go through the same path;
    // none of them shows the window itself.
    visible: false
    // Frameless: the 40 px title band is ours and QuickWindowChrome answers
    // WM_NCHITTEST for it, which is what keeps the native move loop, Snap,
    // double-click-to-maximize, Aero Shake and the system menu working rather
    // than having to be re-implemented in Qt.
    //
    // This is the change the Widgets shell could not make. There, PreviewSurface
    // set Qt::WA_NativeWindow, so a CHILD HWND owned the pixels the title bar
    // drew into and the top-level window was never asked to hit-test them. A
    // QQuickWindow is a single top-level HWND with no native children, so the
    // whole client area belongs to the one window that answers.
    flags: Qt.Window | Qt.FramelessWindowHint | (root.noActivate ? Qt.WindowDoesNotAcceptFocus : 0)
    color: ExoTheme.background
    // Deliberately not qsTr(): the native window title is an identifier here,
    // not copy. The single-instance activation (FindWindowW(nullptr, "ExoSnap"))
    // and the updater's handoff both look the window up by this exact string, so
    // a localized title would silently break both.
    title: "ExoSnap"

    // Close guards live in C++ (models/CloseGuardPolicy via ShellAdapter);
    // this only routes the answer. requestClose() returns false both when a
    // prompt went up and when the close was refused outright.
    onClosing: function(close) {
        if (root.closeApproved)
            return;
        close.accepted = root.shell.requestClose();
    }

    Connections {
        target: root.shell

        function onCloseApproved(): void {
            root.closeApproved = true;
            root.close();
        }

        // Routed through the shell's navigateTo() rather than written straight
        // onto currentPage: this signal is one of five navigation intents, and
        // after QCR-001 all five answer to the same policy. Writing the index
        // here is what let a notification action swap the page under an open
        // Edit workspace while the tabs that claimed to prevent exactly that
        // sat disabled.
        function onNavigateToPageRequested(page: int): void {
            appShell.navigateTo(page);
        }

        // Opened imperatively rather than by binding `visible`: Dialog::accept()
        // closes the popup itself, which would destroy a binding on `visible`
        // and leave the second guard in a chain unable to appear.
        function onCloseGuardChanged(): void {
            if (root.shell.closeGuardActive)
                closeGuardDialog.open();
            else
                closeGuardDialog.close();
        }
    }

    // Win32 non-client behaviour for the frameless shell. The interactive
    // geometry it needs is pushed down from the title bar in AppShell — this
    // object cannot walk a widget tree to find the buttons, so the bar tells it.
    QuickWindowChrome {
        id: windowChrome

        target: root
        titleBarHeight: appShell.titleBarHeight
        // The DWM frame line, kept on the theme rather than left at the system
        // accent so a light theme does not get a dark border and vice versa.
        borderColor: ExoTheme.line

        // Win32 owns the maximized state and QML reads it back from
        // windowMaximized. MEASURED: `visibility = Window.Maximized` on this
        // frameless window is a SetWindowPos onto the work area and nothing more,
        // so the window ends up maximized-sized while Windows still has it
        // normal -- resizable, and with the work-area rect recorded as the rect
        // to un-maximize to. Windows also changes this state on its own (Snap,
        // double-click, Win+arrow), which no Qt-side notion can lead.
        onMaximizeButtonClicked: root.toggleMaximized()
    }

    function toggleMaximized(): void {
        windowChrome.toggleMaximized();
    }

    // Routed through the chrome for the same reason as toggleMaximized(): the
    // window state belongs to Win32 here, and only a native minimize records
    // WPF_RESTORETOMAXIMIZED, which is what brings a maximized window back
    // maximized from the taskbar.
    //
    // The chrome also decides whether this minimize goes to the taskbar or to the
    // tray, and deliberately not this button: SC_MINIMIZE reaches the chrome from
    // the window menu, Win+Down and the taskbar button, none of which QML sees. A
    // check here would leave those routes on the other behaviour.
    function minimizeWindow(): void {
        windowChrome.minimizeWindow();
    }

    ExoConfirmDialog {
        id: closeGuardDialog

        title: root.shell.closeGuardTitle
        bodyText: root.shell.closeGuardBody
        proceedText: root.shell.closeGuardProceedLabel
        cancelText: root.shell.closeGuardCancelLabel
        defaultIsCancel: root.shell.closeGuardDefaultIsCancel
        // Escape resolves to reject(), so dismissing the dialog always means
        // "keep the window open" — never an accidental proceed.
        onAccepted: root.shell.confirmCloseGuard()
        onRejected: root.shell.cancelCloseGuard()
    }

    // The out-of-window toast stack: its own top-level, capture-excluded window,
    // not a child of the shell. A toast about a finished recording is most
    // useful exactly when ExoSnap is not the window in front — behind a
    // fullscreen game, or with the app hidden in the tray.
    OverlayNotificationToast {
        // Named like the four below, because it is one of them for every
        // purpose that matters here: capture-excluded, top-level, invisible to
        // pixel and adapter tests alike. Without a name it never appeared in the
        // Live Verify overlay snapshot at all, so the check that asserts native
        // composition invariants silently covered four windows and reported
        // success — while this one shipped unoperable.
        objectName: "quickOverlayNotificationToast"
        toasts: root.notifications.toastModel
        anchorGeometry: root.notifications.toastAnchorGeometry
        onActionTriggered: function (sequence, action) {
            root.notifications.triggerToastAction(sequence, action);
        }
        onDismissRequested: function (sequence) {
            root.notifications.dismissToast(sequence);
        }
    }

    // ── Windows shell surfaces ───────────────────────────────────────────────
    //
    // The notification-area icon. Not a window and not part of the scene: it
    // lives here because it is a declarative surface of the same application,
    // and because it must outlive the shell window being hidden to it.
    ShellTray {
        tray: root.trayAdapter
    }

    // ── Capture-excluded overlays ────────────────────────────────────────────
    //
    // Four separate top-level windows on the monitor being recorded, not
    // children of the shell: they have to survive the app window being
    // minimised, hidden to the tray or covered by a fullscreen game, which is
    // the situation they exist for. Each one applies WDA_EXCLUDEFROMCAPTURE to
    // itself and stays hidden if that call fails — see CaptureExclusion.
    //
    // WHETHER each is on screen is decided in C++ (OverlayAdapter, over
    // models::OverlayContentPolicy); WHAT it says is bound from the adapters
    // that already own those values. Nothing here decides either.

    OverlayRecording {
        objectName: "quickOverlayRecording"
        monitorGeometry: root.overlays.recordedMonitorGeometry
        overlayState: root.overlays.recordingState
        overlayActive: root.overlays.recordingOverlayActive
        elapsedText: root.recordViewModel.elapsedText
        outputSizeText: root.recordViewModel.outputSizeText
        sourceNameText: root.recordViewModel.sourceName
        showElapsed: root.settingsAdapter.recordingOverlayElapsed
        showOutputSize: root.settingsAdapter.recordingOverlayOutputSize
        showSourceName: root.settingsAdapter.recordingOverlaySourceName
    }

    OverlayDiagnostics {
        objectName: "quickOverlayDiagnostics"
        monitorGeometry: root.overlays.recordedMonitorGeometry
        overlayActive: root.overlays.diagnosticsOverlayActive
        fpsText: root.recordViewModel.capturedFpsText
        dropText: root.recordViewModel.droppedFramesText
        driftText: root.recordViewModel.driftText
        sizeText: root.recordViewModel.outputSizeText
        // "Muted" means the source is NOT part of this recording, which is what
        // the Widgets overlay reported too (its meter callback passed the
        // `*_show` flags, derived from audio_active_*, not the RMS level).
        // Deliberately not derived from the meter: a level of zero is a silent
        // moment, and a glyph that appears every time the user stops talking
        // would report a problem that is not there.
        micMuted: !root.recordViewModel.microphoneEnabled
        sysMuted: !root.recordViewModel.systemAudioEnabled
        showFps: root.settingsAdapter.diagnosticsOverlayFps
        showDrop: root.settingsAdapter.diagnosticsOverlayDrop
        showDrift: root.settingsAdapter.diagnosticsOverlayDrift
        showSize: root.settingsAdapter.diagnosticsOverlaySize
        showMutedSources: root.settingsAdapter.diagnosticsOverlayMutedSources
    }

    OverlayCountdown {
        objectName: "quickOverlayCountdown"
        monitorGeometry: root.overlays.recordedMonitorGeometry
        countdownActive: root.overlays.countdownOverlayActive
        remainingSeconds: root.recordViewModel.countdownRemaining
        durationSeconds: root.recordViewModel.countdownSeconds
        countdownProgress: root.recordViewModel.countdownProgress
    }

    // The one capture-excluded overlay that is deliberately NOT click-through:
    // it is an interactive control surface (ADR 0016), so it takes mouse input
    // while still being kept out of the recording.
    OverlayQuickControlPill {
        objectName: "quickOverlayQuickControls"
        monitorGeometry: root.overlays.recordedMonitorGeometry
        overlayActive: root.overlays.quickControlsActive
        paused: root.recordViewModel.paused
        onPauseResumeRequested: {
            if (root.recordViewModel.paused)
                root.recordViewModel.requestResume();
            else
                root.recordViewModel.requestPause();
        }
        onStopRequested: root.recordViewModel.requestStop()
        onCaptureFrameRequested: root.recordViewModel.requestCaptureFrame()
    }

    // QCR-608. `RecordPreviewAdapter.active` follows the navigation index alone,
    // which stays true while the window is minimized or sitting in the tray —
    // and the capture hub then kept duplicating the desktop at ~66 Hz, arming a
    // scene update per frame for a window that cannot render.
    //
    // Window visibility is the fact, so it is read from the window rather than
    // inferred from an item's `visible`: Qt does not fire
    // ItemVisibleHasChanged on a minimize, which is why the item-level guard the
    // preview already has never saw this case.
    Binding {
        target: root.previewAdapter
        property: "surfaceVisible"
        value: root.visible && root.visibility !== Window.Minimized
    }

    AppShell {
        id: appShell

        anchors.fill: parent
        // AppShell is anchored at the window origin, so its item coordinates and
        // the window coordinates the hit test compares against are the same
        // space — no mapping is needed on the way down.
        chrome: windowChrome
        windowMaximized: windowChrome.windowMaximized
        onMinimizeRequested: root.minimizeWindow()
        onMaximizeRestoreRequested: root.toggleMaximized()
        onCloseRequested: root.close()
        shell: root.shell
        notifications: root.notifications
        recovery: root.recovery
        recordingError: root.recordingError
        crashReport: root.crashReport
        whatsNew: root.whatsNew
        aboutViewModel: root.aboutViewModel
        recordViewModel: root.recordViewModel
        previewAdapter: root.previewAdapter
        settingsAdapter: root.settingsAdapter
        deviceAdapter: root.deviceAdapter
        diagnosticsAdapter: root.diagnosticsAdapter
        logsAdapter: root.logsAdapter
        editSession: root.editSession
        editTimeline: root.editTimeline
        editPlayer: root.editPlayer
        editExport: root.editExport
        benchmarkInteractionActive: root.benchmarkInteractionActive
    }

    // After AppShell's own completion (children complete first), so the shell
    // has already loaded Record and this is a normal destination change rather
    // than an assignment into a half-built stack.
    Component.onCompleted: {
        if (root.landingPage !== ShellAdapter.RecordPage)
            appShell.currentPage = root.landingPage;
    }
}
