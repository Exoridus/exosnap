# app/quick/ExoSnap/Quick/CMakeLists.txt -- the shipping ExoSnap main frontend.
#
# This target IS `exosnap`/`exosnap.exe`: the one production executable, built on
# Qt Quick/QML on top of ProductionBootstrap -> QuickApplication. It was
# developed under the name `exosnap_quick_spike` behind EXOSNAP_BUILD_QUICK_SPIKE
# while the Qt Widgets frontend still shipped; both are gone. There is no runtime
# frontend selector and no second production application.
#
# The directory path mirrors the QML module URI (ExoSnap.Quick -> ExoSnap/Quick
# under the import root app/quick), which is a Qt requirement, not a leftover of
# the migration layout.
# Tests live OUTSIDE this directory, and that is a packaging requirement rather
# than a matter of taste. qt_generate_deploy_qml_app_script() resolves the
# application's QML imports by running qmlimportscanner with `-rootPath` set to
# this directory — it walks the whole tree, not just the files listed in
# QML_FILES. With the QML test cases sitting in a tests/ subdirectory here, their
# `import QtTest` was recorded as an application import and the shipped package
# grew Qt6QuickTest.dll, Qt6Test.dll and a qml/QtTest module: the Qt test
# framework, inside the product. Keep this directory free of any QML that is not
# part of the application.
set(EXOSNAP_QUICK_TESTS_DIR "${PROJECT_SOURCE_DIR}/app/quick/tests")

set_source_files_properties(ExoTheme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_executable(exosnap WIN32
    main.cpp
    ${PROJECT_SOURCE_DIR}/app/assets/brand/exosnap_brand.qrc
    # The mark suite, loaded by the runtime renderer rather than by QML: it is
    # recoloured for the running theme before anything rasterizes it.
    ${PROJECT_SOURCE_DIR}/app/assets/brand/exosnap_marks.qrc
    ${PROJECT_SOURCE_DIR}/app/assets/fonts/exosnap_fonts.qrc
)

qt_add_qml_module(exosnap
    URI ExoSnap.Quick
    DEPENDENCIES
        QtQuick
        QtQuick.Dialogs
        # OverlayCountdown's circle carries a MultiEffect drop shadow.
        QtQuick.Effects
        # EditPlayer draws its play glyph with a Shape rather than a Canvas.
        QtQuick.Shapes
        # DeviceAdapter exposes two QAbstractListModel subclasses as declared QML
        # types; without this qmllint cannot resolve their base type.
        QtQml.Models
    QML_FILES
        AboutMetadataRow.qml
        AboutPage.qml
        AppShell.qml
        CrashReportOverlay.qml
        DeviceAdapterCard.qml
        DeviceCapabilityMatrix.qml
        DeviceCapabilityPanel.qml
        DeviceCapabilityRow.qml
        DeviceCodecChip.qml
        DiagnosticsPage.qml
        DiagnosticsRowCard.qml
        DiagnosticsSectionHeader.qml
        DiagnosticsSelfTestRow.qml
        EditDetailsRail.qml
        EditExportPanel.qml
        EditOverlay.qml
        EditPlayer.qml
        EditTimeline.qml
        ExoBadge.qml
        ExoBrandMark.qml
        ExoBrandWordmark.qml
        ExoButton.qml
        ExoCard.qml
        ExoCheckBox.qml
        ExoChevron.qml
        ExoConfirmDialog.qml
        ExoDisclosure.qml
        ExoGlyph.qml
        ExoIssueCard.qml
        ExoKeyValueTable.qml
        ExoLedgerCard.qml
        ExoLogView.qml
        ExoMenu.qml
        ExoMenuItem.qml
        ExoNavTab.qml
        ExoInfoButton.qml
        ExoNotice.qml
        ExoNumberField.qml
        ExoOverlayCard.qml
        ExoPipelineFlow.qml
        ExoPipelineStepCard.qml
        ExoProgressBar.qml
        ExoScrollBar.qml
        ExoScrollView.qml
        ExoSearchField.qml
        ExoSegmentedControl.qml
        ExoSelect.qml
        ExoLevelMeter.qml
        ExoLevelMeterScale.qml
        ExoSessionTimeline.qml
        ExoSettingRow.qml
        ExoSlider.qml
        ExoSparkline.qml
        ExoStatusPill.qml
        ExoStatusTile.qml
        ExoSwitch.qml
        ExoTextField.qml
        ExoTheme.qml
        ExoTipChip.qml
        HotkeyCaptureField.qml
        LogsPage.qml
        Main.qml
        NotificationBell.qml
        NotificationHub.qml
        OverlayCountdown.qml
        OverlayDiagnostics.qml
        OverlayNotificationToast.qml
        OverlayQuickControlPill.qml
        OverlayRecording.qml
        PreviewMetricsOverlay.qml
        RecordActionButton.qml
        RecordPage.qml
        RecordingErrorOverlay.qml
        RecordSourcePicker.qml
        RecordSourceToggle.qml
        RecordSplitButton.qml
        RecordTransportDock.qml
        RecoveryOverlay.qml
        RegionSelectionOverlay.qml
        SettingsAppearanceSection.qml
        SettingsAudioEncodingSection.qml
        SettingsAudioSourcesSection.qml
        SettingsAudioSourceRow.qml
        SettingsAudioTrackLedger.qml
        SettingsMicrophoneSection.qml
        SettingsChromaSlider.qml
        SettingsDestinationChip.qml
        SettingsDeveloperSection.qml
        SettingsFormatSection.qml
        SettingsHotkeysSection.qml
        SettingsMicDspGroup.qml
        SettingsOutputFolderDialog.qml
        SettingsOutputSection.qml
        SettingsPage.qml
        SettingsPresenceSection.qml
        SettingsPresetBar.qml
        SettingsOverlaysSection.qml
        SettingsPresetFileDialog.qml
        ShellTray.qml
        SettingsPresetNameDialog.qml
        SettingsQualitySection.qml
        SettingsUpdatesSection.qml
        SettingsWebcamSection.qml
        WhatsNewOverlay.qml
        WindowChromeButton.qml
    SOURCES
        AboutViewModelAdapter.cpp
        AboutViewModelAdapter.h
        BlockingSurfaceArbiter.cpp
        BlockingSurfaceArbiter.h
        CaptureExclusion.cpp
        CaptureExclusion.h
        CrashReportAdapter.cpp
        CrashReportAdapter.h
        DeviceAdapter.cpp
        DeviceAdapter.h
        DiagnosticIssueModel.cpp
        DiagnosticIssueModel.h
        PipelineStageModel.cpp
        PipelineStageModel.h
        DiagnosticsAdapter.cpp
        DiagnosticsAdapter.h
        EditExportAdapter.cpp
        EditExportAdapter.h
        EditPlayerAdapter.cpp
        EditPlayerAdapter.h
        EditPlayerWorker.cpp
        EditPlayerWorker.h
        EditSessionAdapter.cpp
        EditSessionAdapter.h
        EditTimelineAdapter.cpp
        EditTimelineAdapter.h
        EditTimelineModels.cpp
        EditTimelineModels.h
        ExoEditPlayerItem.cpp
        ExoEditPlayerItem.h
        LogEntryModel.cpp
        LogEntryModel.h
        LogsAdapter.cpp
        LogsAdapter.h
        NotificationEntryModel.cpp
        NotificationEntryModel.h
        NotificationHubPolicy.h
        NotificationToastModel.cpp
        NotificationToastModel.h
        NotificationsAdapter.cpp
        NotificationsAdapter.h
        ExoPreviewItem.cpp
        ExoPreviewItem.h
        QuickPreviewRgbaConverter.cpp
        QuickPreviewRgbaConverter.h
        RoundedRectClipGeometry.h
        PreviewUpdateScheduler.h
        RecordPreviewAdapter.cpp
        RecordPreviewAdapter.h
        ReadyFrameCaptureService.cpp
        ReadyFrameCaptureService.h
        RecordWebcamFrameProvider.cpp
        RecordWebcamFrameProvider.h
        CaptureTargetStillProvider.cpp
        CaptureTargetStillProvider.h
        OverlayAdapter.cpp
        OverlayAdapter.h
        RecordViewModelAdapter.cpp
        RecordViewModelAdapter.h
        RecordingErrorAdapter.cpp
        RecordingErrorAdapter.h
        RecoveryAdapter.cpp
        RecoveryAdapter.h
        QuickThemeTokens.cpp
        QuickThemeTokens.h
        SettingsAdapter.cpp
        SettingsAdapter.h
        ShellAdapter.cpp
        ShellAdapter.h
        WhatsNewAdapter.cpp
        WhatsNewAdapter.h
        MainWindowAffinity.cpp
        MainWindowAffinity.h
        QuickWindowChrome.cpp
        QuickWindowChrome.h
        QuickWindowGeometry.cpp
        QuickWindowGeometry.h
        ShellPresenceAdapter.cpp
        ShellPresenceAdapter.h
        TaskbarPresence.cpp
        TaskbarPresence.h
        TrayAdapter.cpp
        TrayAdapter.h
        # How QML addresses the rendered mark and the canonical beat.
        Brand.h
)

target_sources(exosnap
    PRIVATE
        QuickApplication.cpp
        QuickApplication.h
        # Harness-only text expansion (QCR-511). Not a QML type: it is installed
        # as a QTranslator before the engine loads and nothing in QML names it.
        PseudoLocalization.cpp
        PseudoLocalization.h
        # Process-level startup/shutdown contract, shared with the Widgets entry
        # point: DLL-search hardening, interrupted-swap self-heal, startup clock,
        # log sink, crash capture, single-instance guard, elevated relaunch.
        ${PROJECT_SOURCE_DIR}/app/bootstrap/ProductionBootstrap.cpp
        ${PROJECT_SOURCE_DIR}/app/bootstrap/ProductionBootstrap.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/EngineLogBridge.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/EngineLogBridge.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/StartupClock.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/StartupTrace.h
        ${PROJECT_SOURCE_DIR}/app/services/ElevatedRelaunch.cpp
        ${PROJECT_SOURCE_DIR}/app/services/ElevatedRelaunch.h
        # ADR 0055: --verify-update-reinstall argv gate, shared with the Widgets
        # entry point.
        ${PROJECT_SOURCE_DIR}/app/services/VerifyReinstallMode.cpp
        ${PROJECT_SOURCE_DIR}/app/services/VerifyReinstallMode.h
        # The dev feed override (--update-base-url): the seam that lets a
        # non-official build exercise its own update check, and therefore the
        # whole app-to-updater handoff, against a controlled feed.
        ${PROJECT_SOURCE_DIR}/app/cli/CommandLineFlags.cpp
        ${PROJECT_SOURCE_DIR}/app/cli/CommandLineFlags.h
        ${PROJECT_SOURCE_DIR}/app/services/SystemAppearance.cpp
        ${PROJECT_SOURCE_DIR}/app/services/SystemAppearance.h
        ${PROJECT_SOURCE_DIR}/app/services/UpdateFeedOverride.cpp
        ${PROJECT_SOURCE_DIR}/app/services/UpdateFeedOverride.h
        ${PROJECT_SOURCE_DIR}/app/models/AboutInfo.cpp
        ${PROJECT_SOURCE_DIR}/app/models/AboutInfo.h
        ${PROJECT_SOURCE_DIR}/app/models/RecordingPresetRegistry.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingPresetRegistry.h
        ${PROJECT_SOURCE_DIR}/app/models/EditContext.h
        ${PROJECT_SOURCE_DIR}/app/models/EditContextFactory.cpp
        ${PROJECT_SOURCE_DIR}/app/models/EditContextFactory.h
        ${PROJECT_SOURCE_DIR}/app/models/CloseGuardPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/CloseGuardPolicy.h
        ${PROJECT_SOURCE_DIR}/app/models/WindowPresencePolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/WindowPresencePolicy.h
        # The one projection the tray, the taskbar button and the in-app
        # transport all read, plus the heartbeat they share and the lease that
        # decides who owns the taskbar progress bar.
        ${PROJECT_SOURCE_DIR}/app/models/ShellPresence.cpp
        ${PROJECT_SOURCE_DIR}/app/models/ShellPresence.h
        ${PROJECT_SOURCE_DIR}/app/models/RecordingPulse.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingPulse.h
        ${PROJECT_SOURCE_DIR}/app/models/TaskbarProgressLease.cpp
        ${PROJECT_SOURCE_DIR}/app/models/TaskbarProgressLease.h
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.h
        ${PROJECT_SOURCE_DIR}/app/models/OverlayContentPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/OverlayContentPolicy.h
        ${PROJECT_SOURCE_DIR}/app/models/HotkeyStartupConflicts.h
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationEvent.h
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationManager.cpp
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationManager.h
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationNames.cpp
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationNames.h
        ${PROJECT_SOURCE_DIR}/app/services/TimelineThumbnailSource.cpp
        ${PROJECT_SOURCE_DIR}/app/services/TimelineThumbnailSource.h
        ${PROJECT_SOURCE_DIR}/app/models/OutputPathPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/OutputPathPolicy.h
        ${PROJECT_SOURCE_DIR}/app/models/OutputPathValidator.cpp
        ${PROJECT_SOURCE_DIR}/app/models/OutputPathValidator.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/AppLog.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/AppLog.h
        # What Windows itself says about our windows: style/ex-style, non-client
        # inset, child HWNDs, display affinity. Shared by --hwnd-audit and the
        # Live Verify window/overlay snapshots so the startup gate and the live
        # observation can never disagree.
        ${PROJECT_SOURCE_DIR}/app/diagnostics/NativeWindowFacts.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/NativeWindowFacts.h
        # ADR 0066: the application binding for the Live Verify control channel.
        # The channel itself lives in exosnap_live_verify; this is the only file
        # that knows what an ExoSnap adapter is.
        QuickLiveVerifySource.cpp
        QuickLiveVerifySource.h
        # The stable product keys settings.get/settings.set speak, and the table
        # that binds each of them to the SettingsAdapter setter the QML control
        # writes to. Not reflection: a key that is not in the table cannot be
        # read or written.
        SettingsAutomationKeys.cpp
        SettingsAutomationKeys.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsController.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsController.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/SessionLedger.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/SessionLedger.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsProbe.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsProbe.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/FixActionDispatcher.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/FixActionDispatcher.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/LogViewPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/LogViewPolicy.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/CrashSessionContext.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/CrashSessionContext.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/ElevationProvider.cpp
        # ADR 0033 present diagnostics. These sources existed since the ETW slice
        # landed but were compiled ONLY by present_provider_tests, so the shipping
        # binary contained the no-op session and could never report a present mode.
        # Wave D compiles them here and defines EXOSNAP_HAS_PRESENTMON below, which
        # is what makes the real ETW consumer reachable at all.
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentMonProvider.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentMonProvider.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentMonEtwSession.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentMonEtwSession.h
        # The ETW boundary, and the only file that names PresentMon. The session above
        # is free of conditional compilation precisely so the product and the tests
        # compile the same lines of it; EXOSNAP_HAS_PRESENTMON now only selects which
        # backend this file provides.
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentMonTraceBackend.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentTraceBackend.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentModeMapping.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentModeMapping.h
        # The DPC/ISR half of the same ADR 0033 opt-in, and the worse case of the same
        # defect: this file was compiled by NO target at all, so the check that names
        # the kernel driver behind "smooth game, stuttery recording" evaluated an
        # absent reading in every build there has ever been.
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DpcLatencyProvider.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DpcLatencyProvider.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentSnapshotOverlay.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/PresentSnapshotOverlay.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/SupportBundle.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/ZipWriter.cpp
        ${PROJECT_SOURCE_DIR}/app/services/SupportBundleService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/SupportBundleService.h
        ${PROJECT_SOURCE_DIR}/app/services/CaptureHubCommandQueue.h
        ${PROJECT_SOURCE_DIR}/app/services/CaptureHubGate.cpp
        ${PROJECT_SOURCE_DIR}/app/services/CaptureHubGate.h
        ${PROJECT_SOURCE_DIR}/app/services/CaptureHubRegistry.cpp
        ${PROJECT_SOURCE_DIR}/app/services/CaptureHubRegistry.h
        ${PROJECT_SOURCE_DIR}/app/services/CaptureSourceHub.cpp
        ${PROJECT_SOURCE_DIR}/app/services/CaptureSourceHub.h
        ${PROJECT_SOURCE_DIR}/app/services/CaptureSourceKey.h
        ${PROJECT_SOURCE_DIR}/app/services/DxgiCaptureHubService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/DxgiCaptureHubService.h
        ${PROJECT_SOURCE_DIR}/app/services/DxgiSourceProducer.cpp
        ${PROJECT_SOURCE_DIR}/app/services/DxgiSourceProducer.h
        ${PROJECT_SOURCE_DIR}/app/services/WgcCaptureHubService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/WgcCaptureHubService.h
        ${PROJECT_SOURCE_DIR}/app/services/WgcSourceProducer.cpp
        ${PROJECT_SOURCE_DIR}/app/services/WgcSourceProducer.h
        ${PROJECT_SOURCE_DIR}/app/services/CaptureTargetStillService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/CaptureTargetStillService.h
        # QCR-110: the production producer of exclusive-fullscreen evidence. Both
        # the Diagnostics card and the recording-admission gate read its snapshot;
        # without it in the shipping target the gate is inert.
        ${PROJECT_SOURCE_DIR}/app/services/WindowEvidenceProbe.cpp
        ${PROJECT_SOURCE_DIR}/app/services/WindowEvidenceProbe.h
        ${PROJECT_SOURCE_DIR}/app/services/RecordingCoordinator.cpp
        ${PROJECT_SOURCE_DIR}/app/services/RecordingAdmission.cpp
        ${PROJECT_SOURCE_DIR}/app/services/RecordingCoordinator.h
        ${PROJECT_SOURCE_DIR}/app/services/RecordingCountdownController.cpp
        ${PROJECT_SOURCE_DIR}/app/services/RecordingCountdownController.h
        ${PROJECT_SOURCE_DIR}/app/services/AtomicFileOps.cpp
        ${PROJECT_SOURCE_DIR}/app/services/AtomicFileOps.h
        ${PROJECT_SOURCE_DIR}/app/services/DisplayNumbering.cpp
        ${PROJECT_SOURCE_DIR}/app/services/DisplayNumbering.h
        ${PROJECT_SOURCE_DIR}/app/services/ScreenPresentation.cpp
        ${PROJECT_SOURCE_DIR}/app/services/ScreenPresentation.h
        ${PROJECT_SOURCE_DIR}/app/services/DisplayIdentityEnumerator.cpp
        ${PROJECT_SOURCE_DIR}/app/services/DisplayIdentityEnumerator.h
        ${PROJECT_SOURCE_DIR}/app/services/DisplayIdentityResolver.cpp
        ${PROJECT_SOURCE_DIR}/app/services/DisplayIdentityResolver.h
        ${PROJECT_SOURCE_DIR}/app/services/GlobalHotkeyService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/GlobalHotkeyService.h
        ${PROJECT_SOURCE_DIR}/app/services/Win32HotkeyRegistrar.h
        # ADR 0012: the Qt-side owner of the update engine. UpdateLaunchPlan alone
        # only provided the pure helpers -- the service itself is what makes the
        # Settings updates card do anything.
        ${PROJECT_SOURCE_DIR}/app/services/UpdateService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/UpdateService.h
        # LaunchUpdater persists the post-update What's New payload.
        ${PROJECT_SOURCE_DIR}/app/services/WhatsNewPayload.cpp
        ${PROJECT_SOURCE_DIR}/app/services/WhatsNewPayload.h
        ${PROJECT_SOURCE_DIR}/app/services/UpdateLaunchPlan.cpp
        # QCR-202: the pure completion gate the check worker is a driver of.
        ${PROJECT_SOURCE_DIR}/app/services/UpdateCheckGate.cpp
        ${PROJECT_SOURCE_DIR}/app/services/UpdateCheckGate.h
        ${PROJECT_SOURCE_DIR}/app/settings/AppSettingsStore.cpp
        ${PROJECT_SOURCE_DIR}/app/settings/AppSettingsStore.h
        ${PROJECT_SOURCE_DIR}/app/settings/RecordingPresetStore.cpp
        ${PROJECT_SOURCE_DIR}/app/settings/RecordingPresetStore.h
        ${PROJECT_SOURCE_DIR}/app/settings/RecoveryManifestStore.cpp
        ${PROJECT_SOURCE_DIR}/app/settings/RecoveryManifestStore.h
        ${PROJECT_SOURCE_DIR}/app/services/RecoveryService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/RecoveryService.h
        ${PROJECT_SOURCE_DIR}/app/settings/RecordingHistoryStore.cpp
        ${PROJECT_SOURCE_DIR}/app/settings/RecordingHistoryStore.h
        ${PROJECT_SOURCE_DIR}/app/models/CaptureTargetPresentation.cpp
        ${PROJECT_SOURCE_DIR}/app/models/CaptureTargetPresentation.h
        ${PROJECT_SOURCE_DIR}/app/viewmodels/RecordViewModel.cpp
        ${PROJECT_SOURCE_DIR}/app/viewmodels/RecordViewModel.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/error_message.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/error_message.h
        # Toolkit-free geometry policy (QRect/QSize only) despite its ui/ home;
        # the Quick window restore reuses it unchanged rather than reimplementing
        # the clamp rules a second time.
        ${PROJECT_SOURCE_DIR}/app/ui/WindowGeometryPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/ui/WindowGeometryPolicy.h
        # The runtime brand renderer and the image provider that publishes it.
        # Not a QML type: QML reaches it through image:// URLs, which is the
        # only icon boundary Qt.labs.platform's SystemTrayIcon offers.
        ShellIconProvider.cpp
        ShellIconProvider.h
        ${PROJECT_SOURCE_DIR}/app/ui/brand/BrandMark.h
        ${PROJECT_SOURCE_DIR}/app/ui/brand/BrandMarkSvg.cpp
        ${PROJECT_SOURCE_DIR}/app/ui/brand/BrandMarkSvg.h
        ${PROJECT_SOURCE_DIR}/app/ui/brand/ShellIconRenderer.cpp
        ${PROJECT_SOURCE_DIR}/app/ui/brand/ShellIconRenderer.h
)

# ---- Frontend A/B benchmark harness ----
# The SAME orchestration the Widgets target compiles, not a Quick-flavoured copy:
# AutoRecordHarness.cpp is Qt Widgets-free above its
# EXOSNAP_ENABLE_VISUAL_TEST_HARNESS guard, which stays undefined here, so the
# Widgets-bound preview entry point is excluded while the shared drive loop is
# not. EXOSNAP_HARNESS_GATE comes from app/CMakeLists.txt: non-Release builds get
# the harness implicitly, Release only with EXOSNAP_BUILD_BENCHMARK_HARNESS=ON.
target_sources(exosnap
    PRIVATE
        "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/QuickAutoRecordHarness.cpp>"
        "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/QuickAutoRecordHarness.h>"
        # The Edit -> Export drive loop. Same gate: it is automation, never a
        # shipping code path, and it reaches the edit adapters the QML binds to.
        "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/QuickAutoEditHarness.cpp>"
        "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/QuickAutoEditHarness.h>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/auto_record/AutoRecordHarness.cpp>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/auto_record/AutoRecordHarness.h>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/auto_record/AutoRecordOptions.cpp>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/benchmark/BenchmarkMetrics.h>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/benchmark/BenchmarkReport.cpp>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/benchmark/BenchmarkReport.h>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/benchmark/BenchmarkEngineMetrics.cpp>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/benchmark/BenchmarkEngineMetrics.h>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/benchmark/BenchmarkEffectiveConfig.cpp>"
        "$<${EXOSNAP_HARNESS_GATE}:${PROJECT_SOURCE_DIR}/app/benchmark/BenchmarkEffectiveConfig.h>"
)

# Deterministic live-pipeline snapshots for the Diagnostics visual states.
# UNGATED, unlike the benchmark harness above, because the function that seeds
# them -- QuickApplication::applyDiagnosticsVisualScenarios -- is itself ungated:
# the Diagnostics and Logs visual scenarios have always been env-configured in
# every configuration. Gating only the snapshots would break the Release link
# rather than remove anything.
target_sources(exosnap
    PRIVATE
        ${PROJECT_SOURCE_DIR}/app/visual_tests/DiagnosticsLiveScenario.cpp
        ${PROJECT_SOURCE_DIR}/app/visual_tests/DiagnosticsLiveScenario.h
)

if(WIN32)
    # The two Windows resource scripts that make this a shippable executable:
    # exosnap.rc carries the three application icons (bootstrap's WM_SETICON path
    # loads IDI_EXOSNAP_APP_ICON out of the EXE's own resources, so a target
    # without them shows a blank taskbar entry), and the generated
    # exosnap_version.rc carries VERSIONINFO, which
    # scripts/build-release-artifacts.ps1 asserts against the release identity.
    target_sources(exosnap
        PRIVATE
            ${PROJECT_SOURCE_DIR}/app/exosnap.rc
            "${PROJECT_BINARY_DIR}/app/exosnap_version.rc"
    )
endif()

# Output into ${PROJECT_BINARY_DIR}/app rather than this subdirectory's own
# binary dir. Two things depend on that location: scripts/build-release-artifacts.ps1
# resolves the built exe as app/<config>/exosnap.exe (multi-config) or
# app/exosnap.exe (Ninja), and every gtest binary is emitted into the same
# directory -- so the Qt/FFmpeg runtime this target stages POST_BUILD is also
# what makes the test executables launchable. While the Widgets frontend still
# existed it played both roles; the Quick target inherits them whole.
set_target_properties(exosnap PROPERTIES
    AUTORCC ON
    OUTPUT_NAME "exosnap"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/app"
)

target_include_directories(exosnap
    PRIVATE
        ${PROJECT_SOURCE_DIR}/app
        ${PROJECT_BINARY_DIR}/app
)

target_compile_definitions(exosnap
    PRIVATE
        NOMINMAX
        EXOSNAP_BUILD_CONFIG="$<CONFIG>"
        # Deliberately WITHOUT EXOSNAP_ENABLE_VISUAL_TEST_HARNESS: that macro is
        # what pulls MainWindow/RecordPage/QApplication into AutoRecordHarness.cpp.
        "$<${EXOSNAP_HARNESS_GATE}:EXOSNAP_ENABLE_AUTO_RECORD_HARNESS=1>"
)

# qmlcachegen-generated translation units instantiate Qt's QJSEngine templates
# in a way MSVC diagnoses as unreachable code under /W4. Keep /WX for project
# code while suppressing that third-party/generated-header diagnostic.
target_compile_options(exosnap
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/wd4702>
)

# ADR 0033 / Wave D: the real in-process PresentMon ETW consumer. Without the
# define, PresentMonEtwSession compiles to the graceful no-op branch and
# IsAvailable() is false forever -- which is exactly the state the shipping
# binary was in, and why present mode, tearing, discarded count and mode flips
# were structurally unreachable rather than merely unavailable.
if(EXOSNAP_WITH_PRESENTMON)
    target_compile_definitions(exosnap PRIVATE EXOSNAP_HAS_PRESENTMON)
    target_link_libraries(exosnap PRIVATE PresentMon::consumer)
endif()

target_link_libraries(exosnap
    PRIVATE
        Qt6::Core
        Qt6::Gui
        # The brand marks are SVG assets recoloured at load, so the renderer
        # rasterizes them rather than a build step doing it ahead of time.
        Qt6::Svg
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
        # The declarative system tray (ShellTray.qml). A labs module, and
        # deliberately behind a thin C++ model so a future incompatibility is a
        # QML-only port.
        Qt6::LabsPlatform
        # Required BY Qt6::LabsPlatform, which documents Qt Widgets as its
        # fallback and imports Qt6Widgets.dll unconditionally -- so the module
        # needs QApplication rather than QGuiApplication. Nothing this
        # application draws is a QWidget, and nothing should: any Widgets type
        # appearing in our own sources means a Quick equivalent was skipped.
        Qt6::Widgets
        exosnap::warnings
        exosnap::capability
        exosnap::update
        # The App -> Updater handoff document. One schema, two processes: this
        # target writes it, exosnap-updater.exe reads it.
        exosnap::update_handoff
        exosnap::crash_capture
        exosnap_output_model_test_support
        exosnap_device_service_test_support
        exosnap_diagnostics_test_support
        exosnap_live_verify
        # The observability serialization boundary (Wave C). The control channel
        # is one consumer of it; the Diagnostics surface reads the same typed
        # owners directly, so neither is downstream of the other.
        exosnap_observability
        exosnap_miniz
        exosnap_engine
        tomlplusplus::tomlplusplus
        d3d11
        d3dcompiler
        dxgi
        # QuickWindowChrome: DWMWA_BORDER_COLOR on the frameless top-level.
        dwmapi
        mfplat
        mf
        mfuuid
        ole32
        runtimeobject
        # ADR 0033: ShellExecuteExW (Shell32) for the elevated relaunch plus the
        # token APIs behind the elevation check (Advapi32); Version.lib backs the
        # file-version queries the update/diagnostics paths make.
        Shell32
        Advapi32
        Version
)

# Windows-N support: /DELAYLOAD turns
# the static Media Foundation imports into demand-load bindings so the process
# starts on an N SKU without the Media Feature Pack. WebcamService::IsMfPresent()
# probes availability with LoadLibraryW before any MF entry point is called, so
# the delay-load failure hook is never reached. Without this the Quick binary
# would regress a documented platform-support property the moment it ships.
if(MSVC)
    target_link_options(exosnap PRIVATE
        /DELAYLOAD:mfplat.dll
        /DELAYLOAD:mf.dll
        /DELAYLOAD:mfreadwrite.dll
    )
    target_link_libraries(exosnap PRIVATE delayimp.lib)
endif()

add_dependencies(exosnap exosnap_build_info)

# ---- Structural guard: a diagnostics producer must be in a shipping target -------
# Wave D found the same defect four times, and this is the cheapest half of it: a
# producer source file that no shipping target compiles. PresentMon* was compiled only
# by present_provider_tests, WindowEvidenceProbe only by the deleted Widgets shell, and
# DpcLatencyProvider by NOTHING -- while Diagnostics recommended as though all three
# were measuring. Nothing in C++ can see a missing caller, but "is this file in the
# binary at all" is a build fact, so it is asserted at configure time rather than left
# to review.
#
# The two targets that may own such a file are the application itself and the
# diagnostics support library it links. A producer that belongs in neither has to say so
# in EXOSNAP_UNSHIPPED_DIAGNOSTICS_PRODUCERS below, with a reason -- the point is that
# the decision is explicit and visible, never silent.
set(EXOSNAP_UNSHIPPED_DIAGNOSTICS_PRODUCERS
    # (empty: every diagnostics producer currently ships)
)
file(GLOB _exosnap_diag_producers CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/app/diagnostics/*Provider.cpp"
    "${PROJECT_SOURCE_DIR}/app/services/*Probe.cpp"
)
set(_exosnap_shipped_sources "")
foreach(_shipping_target IN ITEMS exosnap exosnap_diagnostics_test_support)
    get_target_property(_target_sources ${_shipping_target} SOURCES)
    get_target_property(_target_dir ${_shipping_target} SOURCE_DIR)
    foreach(_source IN LISTS _target_sources)
        # Generator expressions cannot be resolved here; none of them names a producer.
        if(_source MATCHES "\\$<")
            continue()
        endif()
        get_filename_component(_absolute "${_source}" ABSOLUTE BASE_DIR "${_target_dir}")
        list(APPEND _exosnap_shipped_sources "${_absolute}")
    endforeach()
endforeach()
foreach(_producer IN LISTS _exosnap_diag_producers)
    get_filename_component(_producer_name "${_producer}" NAME)
    if(_producer_name IN_LIST EXOSNAP_UNSHIPPED_DIAGNOSTICS_PRODUCERS)
        continue()
    endif()
    if(NOT "${_producer}" IN_LIST _exosnap_shipped_sources)
        message(FATAL_ERROR
            "${_producer_name} produces diagnostics data but is compiled by no shipping target. "
            "Whatever reads it can only ever see the absent/no-op answer. Add it to the exosnap "
            "target (app/quick/ExoSnap/Quick/CMakeLists.txt) or to exosnap_diagnostics_test_support "
            "(app/CMakeLists.txt) -- or, if it deliberately does not ship, to "
            "EXOSNAP_UNSHIPPED_DIAGNOSTICS_PRODUCERS with the reason."
        )
    endif()
endforeach()

# exosnap_engine links the FFmpeg-backed recording path even though this slice
# only consumes its preview transport. Keep the development executable
# launchable from the build tree, matching the shipping target's staging rule.
foreach(_exosnap_ffmpeg_dll IN LISTS EXOSNAP_FFMPEG_DLLS)
    add_custom_command(TARGET exosnap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_exosnap_ffmpeg_dll}" "$<TARGET_FILE_DIR:exosnap>"
        COMMENT "Staging FFmpeg runtime next to exosnap.exe"
        VERBATIM
    )
endforeach()
exosnap_stage_asan_runtime(exosnap)

# ---- Qt runtime staging (build tree) ----
# A correctness requirement, not a convenience. The
# production bootstrap calls SetDefaultDllDirectories(SYSTEM32 |
# APPLICATION_DIR), which deliberately drops PATH from the loader's search
# order — the whole point of the hardening. A build-tree executable that relied
# on Qt being on PATH therefore stops finding qtquickcontrols2plugin.dll's
# dependencies the moment the hardening is active, and fails at
# QQmlApplicationEngine::load rather than at startup. Staging the Qt runtime
# next to the exe puts it back in APPLICATION_DIR, which is where a deployed
# install has it anyway.
#
# --qmldir is required on top of the plain deploy: the QML lives in the module's
# generated resources, so windeployqt cannot discover the QtQuick/Controls/
# Dialogs/Shapes imports by scanning the binary alone.
if(WIN32 AND EXOSNAP_WINDEPLOYQT)
    add_custom_command(TARGET exosnap POST_BUILD
        COMMAND "${EXOSNAP_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}"
            "$<IF:$<CONFIG:Debug>,--debug,--release>" "$<TARGET_FILE:exosnap>"
        COMMENT "Staging Qt runtime + QML imports next to exosnap.exe"
        VERBATIM
    )
elseif(WIN32)
    message(STATUS "windeployqt not found — the Qt Quick executable will not start with DLL hardening active.")
endif()

# Stage crashpad_handler.exe next to the executable so the runtime lookup
# (crash_capture::ResolveHandlerExePath) resolves it from the build tree. Inert
# in the default stub build; the install rule below ships it separately.
if(EXOSNAP_ENABLE_CRASH_CAPTURE AND TARGET crashpad_handler)
    add_dependencies(exosnap crashpad_handler)
    add_custom_command(TARGET exosnap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>"
            "$<TARGET_FILE_DIR:exosnap>/crashpad_handler.exe"
        COMMENT "Staging crashpad_handler.exe next to exosnap.exe"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------------
# Install / deploy — the shipping artifact
# ---------------------------------------------------------------------------
# qt_generate_deploy_qml_app_script(), NOT qt_generate_deploy_app_script(): the
# latter deploys Qt DLLs and non-QML plugins by scanning the binary's imports,
# which for a Qt Quick application misses everything the QML engine loads at
# runtime — QtQuick, QtQuick.Controls (and its style plugin), QtQuick.Dialogs,
# QtQuick.Shapes, QtQml.Models. Those are resolved by qmlimportscanner over the
# module's recorded imports, which only the QML variant runs. The application's
# OWN QML is compiled into the executable's resources by qt_add_qml_module and
# is not part of the deployed tree.
#
# No NO_TRANSLATIONS and no DEPLOY_TOOL_OPTIONS: package content stays exactly
# what the previous shipping target produced, plus the QML runtime.
if(WIN32)
    qt_generate_deploy_qml_app_script(
        TARGET exosnap
        OUTPUT_SCRIPT _exosnap_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )

    install(TARGETS exosnap RUNTIME DESTINATION .)

    # Deploy the Qt runtime (DLLs + qt.conf) flat next to exosnap.exe rather than
    # into a bin/ subdirectory. exosnap.exe installs to the prefix root, and a
    # root executable on Windows cannot load Qt DLLs from bin/ — a flat layout is
    # required for the portable package to launch. Qt's deploy support honors a
    # pre-set QT_DEPLOY_BIN_DIR (it only defaults the value when undefined), and
    # this install(CODE) runs in the same cmake_install.cmake pass as the deploy
    # script below, so the override is in effect when the script runs.
    #
    # QT_DEPLOY_QML_DIR is left at its default "qml". The generated qt.conf names
    # it, so the deployed application resolves its QML imports relative to its own
    # directory and never from the build tree or a developer Qt installation.
    install(CODE "set(QT_DEPLOY_BIN_DIR \".\")")

    # Contract guard for the generated script installed below. Qt writes that
    # script to call qt6_deploy_qml_imports(), which only exists if
    # Qt6QmlDeploySupport.cmake made it into the generated
    # QtDeploySupport-<CONFIG>.cmake. Qt collects that list off the Qt6::Core
    # target, so it depends on every find_package(Qt6) in the tree sharing ONE
    # Qt6::Core — see CMAKE_FIND_PACKAGE_TARGETS_GLOBAL in the top-level
    # CMakeLists.txt. Without this check the regression surfaces inside Qt's own
    # generated script as "Unknown CMake command", which names neither the cause
    # nor the fix, and the shipped tree would silently be missing the entire QML
    # runtime. Re-including the deploy support is free: it only defines
    # functions.
    install(CODE "
include(\"${QT_DEPLOY_SUPPORT}\")
if(NOT COMMAND qt6_deploy_qml_imports)
    message(FATAL_ERROR
        \"Qt QML deployment support is missing from the generated deploy script, so the \"
        \"installed tree would not contain the Qt Quick runtime. Check that the top-level \"
        \"CMakeLists.txt still sets CMAKE_FIND_PACKAGE_TARGETS_GLOBAL before any \"
        \"find_package(Qt6).\")
endif()")

    install(SCRIPT ${_exosnap_deploy_script})
endif()

# EXOSNAP_CONFIG_DIR is where the QML bytecode cache ends up (see
# AlignQmlDiskCacheWithConfigDir), and the suite runs with one config dir for the
# whole run. Two application processes sharing it race on the same .qmlc files,
# and a document read while another process is still writing it loads without its
# signal connections -- which surfaces as an unrelated navigation assertion
# rather than as a cache error. Every test that launches the application
# concurrently therefore needs a config dir of its own.
add_test(NAME quick.qml.about_smoke COMMAND exosnap --smoke-test)
set_tests_properties(quick.qml.about_smoke PROPERTIES
    LABELS "quick"
    ENVIRONMENT "EXOSNAP_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-config/about_smoke"
)

# The four non-Record destinations are loaded by URL so their documents stay out
# of the startup compile (QCR-615). That the pages are absent until navigation,
# that setSource() actually delivered every adapter, and that Diagnostics' two
# navigation signals still reach the shell are all only observable in the real
# shell -- AppShell requires fifteen C++ adapters and cannot be built in a QML
# test. Runs offscreen with the rest of the suite; no window reaches the screen.
add_test(NAME quick.qml.navigation_lifecycle COMMAND exosnap --navigation-lifecycle-test)
set_tests_properties(quick.qml.navigation_lifecycle PROPERTIES
    LABELS "quick"
    ENVIRONMENT "EXOSNAP_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-config/navigation_lifecycle"
)

# An isolated run has to isolate Qt's QML bytecode cache as well — see the
# script for why EXOSNAP_CONFIG_DIR does not reach it on its own.
add_test(
    NAME quick.qml.disk_cache_isolation
    COMMAND ${CMAKE_COMMAND}
        -DEXOSNAP_EXE=$<TARGET_FILE:exosnap>
        -DSCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/qml-cache-isolation
        -P ${PROJECT_SOURCE_DIR}/cmake/tests/assert_qml_cache_isolated.cmake
)
set_tests_properties(quick.qml.disk_cache_isolation PROPERTIES LABELS "quick")

# The fixed-dark canon for the five capture-excluded overlays, checked at the
# source because nothing downstream can observe it: those windows defeat every
# form of capture, and the harness only sees their scene graph.
add_test(
    NAME quick.qml.overlays_are_fixed_dark
    COMMAND ${CMAKE_COMMAND}
        -DQML_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${PROJECT_SOURCE_DIR}/cmake/tests/assert_overlays_are_fixed_dark.cmake
)
set_tests_properties(quick.qml.overlays_are_fixed_dark PROPERTIES LABELS "quick")

qt_add_executable(record_controls_qml_tests
    ${EXOSNAP_QUICK_TESTS_DIR}/qml_test_main.cpp
    ${PROJECT_SOURCE_DIR}/app/assets/fonts/exosnap_fonts.qrc
)

# AUTORCC, because the .qrc above is only a list of files until something compiles
# it. `exosnap` gets it from a set_target_properties() call of its own;
# qt_standard_project_setup() does NOT set CMAKE_AUTORCC, so without this line the
# resource is silently dropped, qrc:/fonts/* does not exist, and ExoTheme falls back
# to Segoe UI -- leaving the elision and single-line assertions in these suites
# measuring a font the product never ships.
set_target_properties(record_controls_qml_tests PROPERTIES AUTORCC ON)

qt_add_qml_module(record_controls_qml_tests
    URI ExoSnap.Quick.TestControls
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/ExoSnap/Quick/TestControls"
    # QuickThemeTokens exposes QColor properties; without this qmllint cannot
    # resolve their type and reports every token line.
    DEPENDENCIES
        QtQuick
        # ExoGlyph draws with Shape/PathSvg. Listing DEPENDENCIES at all turns
        # off auto-detection for this module, so an import missing from the list
        # is an import qmllint cannot resolve — and a type it cannot fully
        # resolve stops publishing its enum, which surfaced here as every
        # `ExoGlyph.Mic` in the tested components being reported as an unknown
        # member while the same lines linted clean in the shipping module.
        QtQuick.Shapes
    # ExoTheme.qml resolves every colour through the QuickThemeTokens C++
    # singleton. Compiling that class into this module too registers it under
    # this URI, so the tested components see the same tokens the shipping module
    # does. Without it the tests ran against a theme whose every colour was an
    # undefined reference, and `all_qmllint` reported one unqualified-access
    # error per token line.
    SOURCES
        QuickThemeTokens.cpp
        QuickThemeTokens.h
        # OverlayDiagnostics gates its own visibility on CaptureExclusion.granted,
        # so the type has to exist for the overlay to load at all. Nothing in the
        # token tests shows the window — `granted` stays false and the delegates
        # exist regardless, which is the cost QCR-606 is about.
        CaptureExclusion.cpp
        CaptureExclusion.h
        # WhatsNewOverlay.qml takes the adapter as a REQUIRED typed property, so
        # the type has to be registered under this URI for the document to load
        # at all. It carries no policy — the notes, the mode and the two user
        # inputs — which is what makes the surface testable from QML: the test
        # calls present() and asserts what the card does with it.
        WhatsNewAdapter.cpp
        WhatsNewAdapter.h
    QML_FILES
        # QCR-709: the capability row is one line by contract, which is a layout
        # fact about how its value label behaves once the row runs out of width.
        DeviceCapabilityRow.qml
        DeviceCodecChip.qml
        ExoButton.qml
        ExoChevron.qml
        # The 24 px interaction target and the keyboard toggle are geometry and
        # input, so they are asserted rather than judged on a screenshot.
        ExoCheckBox.qml
        # The shared confirm. Whether it holds the keyboard is the whole contract:
        # a modal that never takes focus leaves Escape and Return answering the
        # page behind it, and nothing about that is visible in a screenshot.
        ExoConfirmDialog.qml
        ExoDisclosure.qml
        # Severity carried by something other than colour (QCR-507/508): which
        # glyph a tone resolves to, and what the accessible name says.
        ExoInfoButton.qml
        ExoNotice.qml
        # The suffix formatting (textFromValue) and its inverse (valueFromText)
        # only matter together -- a test module built without the paired parser
        # would exercise a SpinBox that never showed the bug.
        ExoNumberField.qml
        # The tile's head badge and codec chips draw a small tone-tinted pill,
        # same as the diagnostics id/Elev chips elsewhere.
        ExoBadge.qml
        # The collapsed row's own disclosure re-uses the shared chevron rather
        # than ExoDisclosure, so it needs the chevron type resolved too.
        ExoLedgerCard.qml
        ExoSessionTimeline.qml
        ExoSparkline.qml
        ExoStatusTile.qml
        # The shared overlay card and the two components it draws itself from.
        # Its layout invariants — the card staying out of the shell's title
        # band, and the persistent decision strip surviving a scrolling body —
        # are geometry, so they are asserted here rather than judged on a
        # screenshot.
        ExoOverlayCard.qml
        ExoScrollBar.qml
        ExoScrollView.qml
        # The record transport's two controls draw their icons with ExoGlyph, so
        # the test module has to carry it as well — without it qmllint resolves
        # nothing inside either component and reports the whole icon block.
        ExoGlyph.qml
        # The log viewer, for its selection contract: a selection identifies
        # entries by sequence, so it has to survive an append, front eviction
        # and a filter change without ever sliding onto another entry. Driven
        # here against a plain ListModel — the real one is a C++ proxy the test
        # module cannot construct, and none of what is asserted is about it.
        ExoLogView.qml
        # The title band's destination. Its width has to stay put while the
        # selection moves across the row, which is geometry rather than
        # something a screenshot of one selected state can show.
        ExoNavTab.qml
        ExoSegmentedControl.qml
        # Elision needs a real constrained width, which is a layout fact and so
        # is asserted rather than judged on a screenshot.
        ExoStatusPill.qml
        # The switch knob is moved by an XAnimator inside a Behavior on a bound
        # `x`. Whether the binding survives that is exactly the kind of question
        # a comment cannot answer.
        ExoSwitch.qml
        # The only shared control with neither a focusPolicy nor an
        # activeFocusOnTab, and it paints a focus ring. Whether Tab can reach it
        # is a question only a Tab press answers.
        ExoSlider.qml
        # The shared text input owns its draft while it has the focus. That
        # contract is the whole fix, and it is one property write away from
        # being undone.
        ExoTextField.qml
        ExoTheme.qml
        # QCR-604: the pipeline cards are delegates over a model with a stable
        # per-stage identity now, so what has to be asserted is that a value
        # update reaches the SAME card object.
        ExoPipelineFlow.qml
        ExoPipelineStepCard.qml
        # QCR-606: the HUD's token list is keyed on the content policy, not on
        # the measured values.
        OverlayDiagnostics.qml
        PreviewMetricsOverlay.qml
        RecordActionButton.qml
        RecordSourceToggle.qml
        # The Maximize button reconstructs hover, press and click from the
        # non-client message stream, because returning HTMAXBUTTON hands its
        # rectangle to Windows and Qt stops delivering mouse events there. That
        # substitution is state, not pixels, so it is asserted here.
        WindowChromeButton.qml
        # The release-notes surface. Both entry points, the one-time consumption
        # rules and the focus hand-back are behaviour rather than pixels, so they
        # are asserted here; --overlay-visual-state photographs the document.
        WhatsNewOverlay.qml
)

target_compile_definitions(record_controls_qml_tests
    PRIVATE
        QUICK_TEST_SOURCE_DIR="${EXOSNAP_QUICK_TESTS_DIR}"
)

# QuickThemeTokens.cpp reads the shared theme table, which lives with the Widgets
# frontend's theme code and is header-only.
target_include_directories(record_controls_qml_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/app"
)

target_compile_options(record_controls_qml_tests
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/wd4702>
)

target_link_libraries(record_controls_qml_tests
    PRIVATE
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickTest
        # CaptureExclusion logs its verdict through AppLog.
        exosnap_app_log_test_support
        exosnap::warnings
)

add_test(NAME quick.qml.record_controls COMMAND record_controls_qml_tests)
get_filename_component(_exosnap_quick_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
# QML2_IMPORT_PATH is the Qt 5 name and Qt 6 ignores it, so this test used to run
# with the Qt install's qml/ tree invisible. Finding the modules is only half of
# it: a QML plugin DLL there links Qt modules that live in Qt's bin/, and the
# staged copies next to the test binary do not include them — so the plugin
# resolves and then fails to load. Qt's bin/ on PATH covers that.
set_tests_properties(quick.qml.record_controls PROPERTIES
    LABELS "quick"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${_exosnap_quick_qt_prefix}/qml"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_exosnap_quick_qt_prefix}/bin"
)

# The Edit surface's keyboard contract (QCR-504). A second QML test executable
# rather than more files in the one above, because EditTimeline.qml takes three
# REQUIRED C++-typed adapters: they cannot be stubbed from QML, and seeding a
# session needs a plain C++ call, so the runner builds and seeds them and hands
# them over as context properties.
qt_add_executable(edit_timeline_qml_tests
    ${EXOSNAP_QUICK_TESTS_DIR}/edit_timeline_qml_test_main.cpp
    ${PROJECT_SOURCE_DIR}/app/assets/fonts/exosnap_fonts.qrc
)

# AUTORCC, because the .qrc above is only a list of files until something compiles
# it. `exosnap` gets it from a set_target_properties() call of its own;
# qt_standard_project_setup() does NOT set CMAKE_AUTORCC, so without this line the
# resource is silently dropped, qrc:/fonts/* does not exist, and ExoTheme falls back
# to Segoe UI -- leaving the elision and single-line assertions in these suites
# measuring a font the product never ships.
set_target_properties(edit_timeline_qml_tests PROPERTIES AUTORCC ON)

qt_add_qml_module(edit_timeline_qml_tests
    URI ExoSnap.Quick.EditTestControls
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/ExoSnap/Quick/EditTestControls"
    DEPENDENCIES
        QtQuick
        # The timeline's preview-state hint draws ExoGlyph, which is built on
        # Shape/PathSvg; an unlisted import is one qmllint cannot resolve.
        QtQuick.Shapes
    # Compiling the adapters into this module registers them under this URI, so
    # EditTimeline.qml resolves the same types it does in the shipping module.
    # Same reason QuickThemeTokens is here: every colour in the strip goes
    # through ExoTheme, which reads that singleton.
    SOURCES
        EditPlayerAdapter.cpp
        EditPlayerAdapter.h
        EditPlayerWorker.cpp
        EditPlayerWorker.h
        EditSessionAdapter.cpp
        EditSessionAdapter.h
        EditTimelineAdapter.cpp
        EditTimelineAdapter.h
        EditTimelineModels.cpp
        EditTimelineModels.h
        # EditPlayerAdapter's frame sink hands decoded frames to the scene-graph
        # item, so the two link together. The item is never instantiated here —
        # EditTimeline.qml does not draw the picture — but the symbol has to
        # resolve.
        ExoEditPlayerItem.cpp
        ExoEditPlayerItem.h
        QuickPreviewRgbaConverter.cpp
        QuickPreviewRgbaConverter.h
        QuickThemeTokens.cpp
        QuickThemeTokens.h
        ${PROJECT_SOURCE_DIR}/app/services/TimelineThumbnailSource.cpp
        ${PROJECT_SOURCE_DIR}/app/services/TimelineThumbnailSource.h
    QML_FILES
        EditPlayer.qml
        EditTimeline.qml
        ExoGlyph.qml
        ExoTheme.qml
)

# A sibling directory, not a subdirectory of the shared tests/ one: QUICK_TEST_MAIN
# scans its source dir RECURSIVELY for tst_*.qml, so the two runners would each
# try to load the other's cases against a module that does not export them.
target_compile_definitions(edit_timeline_qml_tests
    PRIVATE
        NOMINMAX
        QUICK_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}/app/quick/tests_edit"
)

target_include_directories(edit_timeline_qml_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/app"
)

# Same as record_controls_qml_tests: QUICK_TEST_MAIN_WITH_SETUP pulls in Qt QML
# headers that trip C4702 under this project's /WX.
target_compile_options(edit_timeline_qml_tests
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/wd4702>
)

target_link_libraries(edit_timeline_qml_tests
    PRIVATE
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickTest
        exosnap_app_log_test_support
        exosnap_engine
        exosnap::warnings
)

add_test(NAME quick.qml.edit_timeline COMMAND edit_timeline_qml_tests)
set_tests_properties(quick.qml.edit_timeline PROPERTIES
    LABELS "quick"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${_exosnap_quick_qt_prefix}/qml"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_exosnap_quick_qt_prefix}/bin"
)

# The Record source picker's QML contract. A third QML test executable, for the
# reason the edit runner states: RecordSourcePicker takes a REQUIRED, C++-typed
# adapter (RecordViewModelAdapter) that cannot be stubbed from QML, so the
# runner builds one over a seeded RecordViewModel and hands it over as a context
# property. Its tests live in their own tests_record/ directory because
# QUICK_TEST_MAIN scans its source dir recursively and the runners must not load
# each other's cases.
qt_add_executable(record_source_picker_qml_tests
    ${EXOSNAP_QUICK_TESTS_DIR}/record_picker_qml_test_main.cpp
    ${PROJECT_SOURCE_DIR}/app/assets/fonts/exosnap_fonts.qrc
)

set_target_properties(record_source_picker_qml_tests PROPERTIES AUTORCC ON)

qt_add_qml_module(record_source_picker_qml_tests
    URI ExoSnap.Quick.RecordPickerTestControls
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/ExoSnap/Quick/RecordPickerTestControls"
    DEPENDENCIES
        QtQuick
        # ExoGlyph draws with Shape/PathSvg; without this qmllint cannot resolve
        # its base type and stops publishing the Kind enum.
        QtQuick.Shapes
    SOURCES
        RecordViewModelAdapter.cpp
        RecordViewModelAdapter.h
        QuickThemeTokens.cpp
        QuickThemeTokens.h
    QML_FILES
        ExoButton.qml
        ExoGlyph.qml
        ExoScrollBar.qml
        ExoSearchField.qml
        ExoSegmentedControl.qml
        ExoTheme.qml
        RecordSourcePicker.qml
)

target_compile_definitions(record_source_picker_qml_tests
    PRIVATE
        NOMINMAX
        QUICK_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}/app/quick/tests_record"
)

target_include_directories(record_source_picker_qml_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/app"
)

target_compile_options(record_source_picker_qml_tests
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/wd4702>
)

target_link_libraries(record_source_picker_qml_tests
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickTest
        exosnap_record_viewmodel_test_support
        exosnap::capability
        exosnap_engine
)

add_test(NAME quick.qml.record_source_picker COMMAND record_source_picker_qml_tests)
set_tests_properties(quick.qml.record_source_picker PROPERTIES
    LABELS "quick"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${_exosnap_quick_qt_prefix}/qml"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_exosnap_quick_qt_prefix}/bin"
)

add_test(NAME quick.qml.record_preview_smoke COMMAND exosnap --preview-smoke-test)
set_tests_properties(quick.qml.record_preview_smoke PROPERTIES
    LABELS "quick;live"
    ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:windows"
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 3
)

# Walks every navigation destination, reports the pointer over each control that
# declares a pointing hand, and compares what Qt thinks the window cursor is with
# what the OS is actually carrying. Needs the native platform plugin and a real
# HWND, so it is labelled live and runs alone -- GetCursor() reads state the
# machine's own pointer shares.
add_test(NAME quick.qml.cursor_audit COMMAND exosnap --cursor-audit)
set_tests_properties(quick.qml.cursor_audit PROPERTIES
    LABELS "quick;live"
    ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:windows"
    RUN_SERIAL TRUE
)

add_test(NAME quick.qml.record_preview_lifecycle COMMAND exosnap --preview-lifecycle-test)
set_tests_properties(quick.qml.record_preview_lifecycle PROPERTIES
    LABELS "quick;live"
    ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:windows"
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 3
)

exosnap_add_gtest(
    NAME record_viewmodel_qml_adapter_tests
    TEST_PREFIX quick_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_record_viewmodel_adapter.cpp
        RecordViewModelAdapter.cpp
        RecordViewModelAdapter.h
        RecordWebcamFrameProvider.cpp
        RecordWebcamFrameProvider.h
        CaptureTargetStillProvider.cpp
        CaptureTargetStillProvider.h
    LIBRARIES
        Qt6::Core
        Qt6::Quick
        exosnap_record_viewmodel_test_support
        exosnap::capability
        exosnap_engine
)

target_include_directories(record_viewmodel_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(record_viewmodel_qml_adapter_tests
    PRIVATE
        NOMINMAX
)

exosnap_add_gtest(
    NAME capture_target_still_tests
    TEST_PREFIX quick_stills.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_capture_target_still_service.cpp
        ${PROJECT_SOURCE_DIR}/app/services/CaptureTargetStillService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/CaptureTargetStillService.h
        ${PROJECT_SOURCE_DIR}/app/services/WgcSourceProducer.cpp
        ${PROJECT_SOURCE_DIR}/app/services/WgcSourceProducer.h
    LIBRARIES
        exosnap_app_log_test_support
        Qt6::Core
        Qt6::Gui
        exosnap_engine
        d3d11.lib
        dxgi.lib
        runtimeobject.lib
        ole32.lib
    # A real desktop session with DWM composition: the probe window has to be
    # rendered before it can be captured.
    LABELS live
)

exosnap_add_gtest(
    NAME about_viewmodel_qml_adapter_tests
    TEST_PREFIX quick_about_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_about_viewmodel_adapter.cpp
        AboutViewModelAdapter.cpp
        AboutViewModelAdapter.h
        ${PROJECT_SOURCE_DIR}/app/models/AboutInfo.cpp
        ${PROJECT_SOURCE_DIR}/app/models/AboutInfo.h
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        exosnap::update
)

target_include_directories(about_viewmodel_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
        ${PROJECT_BINARY_DIR}/app
)

target_compile_definitions(about_viewmodel_qml_adapter_tests
    PRIVATE
        NOMINMAX
        EXOSNAP_BUILD_CONFIG="$<CONFIG>"
)

add_dependencies(about_viewmodel_qml_adapter_tests exosnap_build_info)

exosnap_add_gtest(
    NAME settings_qml_adapter_tests
    TEST_PREFIX quick_settings_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_settings_adapter.cpp
        # The automation key table lives with the adapter it binds to, so a
        # renamed setter is a compile error in both at once.
        ${EXOSNAP_QUICK_TESTS_DIR}/test_settings_automation_keys.cpp
        SettingsAutomationKeys.cpp
        SettingsAutomationKeys.h
        SettingsAdapter.cpp
        SettingsAdapter.h
        QuickThemeTokens.cpp
        QuickThemeTokens.h
        ${PROJECT_SOURCE_DIR}/app/models/OverlayContentPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/OverlayContentPolicy.h
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        exosnap_output_model_test_support
        exosnap::capability
        exosnap_engine
)

target_include_directories(settings_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(settings_qml_adapter_tests PRIVATE NOMINMAX)

# The appearance x accent contrast gate. Reads the resolved tokens through
# QuickThemeTokens rather than the table, so a derivation rule that changes a
# colour is caught here too, and needs nothing else from the application.
exosnap_add_gtest(
    NAME theme_contrast_tests
    TEST_PREFIX quick_theme_contrast.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_theme_contrast.cpp
        QuickThemeTokens.cpp
        QuickThemeTokens.h
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
)

target_include_directories(theme_contrast_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(theme_contrast_tests PRIVATE NOMINMAX)

# The text-expansion rule the 860x700 layout gate renders with. A pure function
# plus a QTranslator subclass, so it needs nothing from the application.
exosnap_add_gtest(
    NAME pseudo_localization_tests
    TEST_PREFIX quick_pseudo_localization.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_pseudo_localization.cpp
        PseudoLocalization.cpp
        PseudoLocalization.h
    LIBRARIES
        Qt6::Core
)

target_include_directories(pseudo_localization_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(pseudo_localization_tests PRIVATE NOMINMAX)

exosnap_add_gtest(
    NAME device_qml_adapter_tests
    TEST_PREFIX quick_device_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_device_adapter.cpp
        DeviceAdapter.cpp
        DeviceAdapter.h
    # exosnap_app_log_test_support: DeviceAdapter logs the device-rescan-start/end
    # perf milestones via AppLog, same as the Widgets DevicePage.
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        exosnap_app_log_test_support
        exosnap::capability
        exosnap_engine
)

target_include_directories(device_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(device_qml_adapter_tests PRIVATE NOMINMAX)

exosnap_add_gtest(
    NAME ready_frame_capture_tests
    TEST_PREFIX quick_ready_frame.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_ready_frame_capture_service.cpp
        ReadyFrameCaptureService.cpp
        ReadyFrameCaptureService.h
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        exosnap_engine
        d3d11
        d3dcompiler
        dxgi
)

target_include_directories(ready_frame_capture_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(ready_frame_capture_tests PRIVATE NOMINMAX)

# Whether a HoverHandler's cursorShape reaches the window at all. A QML test can
# only read the handler's own property, which proves the declaration and nothing
# about the cursor a user sees; this drives a real QQuickWindow with posted hover
# events and asserts the shape the window ends up carrying.
exosnap_add_gtest(
    NAME hover_cursor_tests
    TEST_PREFIX quick_hover_cursor.
    SOURCES ${EXOSNAP_QUICK_TESTS_DIR}/test_hover_cursor.cpp
    LIBRARIES
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::Test
)

# The other half of the same question, and the half that can fail: a real HWND
# driven by WM_MOUSEMOVE, asserting the cursor the OS ends up carrying. Needs the
# native platform plugin -- under offscreen there is no HWND to send to and the
# cases skip, which is why this is a separate binary with its own environment.
exosnap_add_gtest(
    NAME hover_cursor_native_tests
    TEST_PREFIX quick_hover_cursor_native.
    SOURCES ${EXOSNAP_QUICK_TESTS_DIR}/test_hover_cursor_native.cpp
    LABELS "quick;live"
    LIBRARIES
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
)

set_tests_properties(quick_hover_cursor_native.hover_cursor_native_tests PROPERTIES
    ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:windows"
    RUN_SERIAL TRUE
)

# Diagnostics/Logs presentation policy extracted out of the Widgets DiagnosticsPage,
# LogsPage and MainWindow. Registered here rather than in app/CMakeLists.txt so the
# Phase-B migration keeps its shared-file footprint to the Quick module.
exosnap_add_gtest(
    NAME diagnostics_policy_tests
    TEST_PREFIX quick_diagnostics_policy.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_diagnostics_policy.cpp
        # The live summary tiles, driven by the SAME fixtures the visual harness
        # seeds, so a capture and these assertions describe one thing.
        ${EXOSNAP_QUICK_TESTS_DIR}/test_diagnostics_live_tiles.cpp
        # The per-session record of what was measured: the entry debounce and the
        # permanence rule the ledger cards and the frozen session report rest on.
        ${EXOSNAP_QUICK_TESTS_DIR}/test_session_ledger.cpp
        ${PROJECT_SOURCE_DIR}/app/visual_tests/DiagnosticsLiveScenario.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsController.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/SessionLedger.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsProbe.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/FixActionDispatcher.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        exosnap_diagnostics_test_support
        # So the live tiles and pipeline.snapshot can be asserted against each
        # other in one place: they read the same snapshot, and the test is what
        # keeps that from quietly becoming two readings of it.
        exosnap_observability
        exosnap::capability
        exosnap_engine
)

target_include_directories(diagnostics_policy_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(diagnostics_policy_tests PRIVATE NOMINMAX)

# The preview redraw gate. Header-only and Qt-free apart from quint64, so the
# scheduling contract is pinned without a scene graph, a GPU or an event loop.
exosnap_add_gtest(
    NAME preview_update_scheduler_tests
    TEST_PREFIX quick_preview_scheduler.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_preview_update_scheduler.cpp
    LIBRARIES
        Qt6::Core
)

target_include_directories(preview_update_scheduler_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(preview_update_scheduler_tests PRIVATE NOMINMAX)

exosnap_add_gtest(
    NAME preview_percentile_tests
    TEST_PREFIX quick_preview_percentile.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_preview_percentile.cpp
    LIBRARIES
        Qt6::Core
)

target_include_directories(preview_percentile_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(preview_percentile_tests PRIVATE NOMINMAX)

exosnap_add_gtest(
    NAME webcam_chroma_key_tests
    TEST_PREFIX quick_webcam_chroma_key.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_webcam_chroma_key.cpp
        RecordWebcamFrameProvider.cpp
        RecordWebcamFrameProvider.h
        CaptureTargetStillProvider.cpp
        CaptureTargetStillProvider.h
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
)

target_include_directories(webcam_chroma_key_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(webcam_chroma_key_tests PRIVATE NOMINMAX)

exosnap_add_gtest(
    NAME log_view_policy_tests
    TEST_PREFIX quick_log_policy.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_log_view_policy.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/LogViewPolicy.cpp
    LIBRARIES
        Qt6::Core
        exosnap_app_log_test_support
)

target_include_directories(log_view_policy_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(log_view_policy_tests PRIVATE NOMINMAX)

exosnap_add_gtest(
    NAME log_entry_model_tests
    TEST_PREFIX quick_log_model.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_log_entry_model.cpp
        LogEntryModel.cpp
        LogEntryModel.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/LogViewPolicy.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Qml
        exosnap_app_log_test_support
)

target_include_directories(log_entry_model_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(log_entry_model_tests PRIVATE NOMINMAX)

exosnap_add_gtest(
    NAME diagnostics_qml_adapter_tests
    TEST_PREFIX quick_diagnostics_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_diagnostics_adapter.cpp
        ${EXOSNAP_QUICK_TESTS_DIR}/test_pipeline_stage_model.cpp
        DiagnosticIssueModel.cpp
        DiagnosticIssueModel.h
        PipelineStageModel.cpp
        PipelineStageModel.h
        DiagnosticsAdapter.cpp
        DiagnosticsAdapter.h
        LogEntryModel.cpp
        LogEntryModel.h
        LogsAdapter.cpp
        LogsAdapter.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsController.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/SessionLedger.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsProbe.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/FixActionDispatcher.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/LogViewPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/ElevationProvider.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/SupportBundle.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/ZipWriter.cpp
        ${PROJECT_SOURCE_DIR}/app/services/SupportBundleService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/SupportBundleService.h
        ${PROJECT_SOURCE_DIR}/app/models/CaptureTargetPresentation.cpp
        ${PROJECT_SOURCE_DIR}/app/services/DisplayNumbering.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        exosnap_app_log_test_support
        exosnap_diagnostics_test_support
        exosnap_miniz
        exosnap::capability
        exosnap::update
        exosnap_engine
)

target_include_directories(diagnostics_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
        ${PROJECT_BINARY_DIR}/app
)

target_compile_definitions(diagnostics_qml_adapter_tests
    PRIVATE
        NOMINMAX
        EXOSNAP_BUILD_CONFIG="$<CONFIG>"
)

add_dependencies(diagnostics_qml_adapter_tests exosnap_build_info)

# Edit-surface policy extracted out of the Widgets EditExportPage, EditTimeline
# and ExportPanel: trim snapping, marker thinning, export path/progress rules and
# the timeline row budget. Registered here so the Phase-D migration keeps its
# shared-file footprint to the Quick module.
# Close-guard policy + its asynchronous routing. Registered here because both
# the policy and the adapter are part of the Quick shell's safety surface: the
# Widgets version could read a QMessageBox answer inline, so the ordering was
# never separately testable.
exosnap_add_gtest(
    NAME shell_adapter_tests
    TEST_PREFIX quick_shell_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_shell_adapter.cpp
        ShellAdapter.cpp
        ShellAdapter.h
        ${PROJECT_SOURCE_DIR}/app/models/CloseGuardPolicy.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Qml
)

target_include_directories(shell_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(shell_adapter_tests PRIVATE NOMINMAX)

# What the shell window does when it is not wanted on screen: where a minimize
# goes, and whether the window is in a capture. Both decisions are read on paths
# that own an HWND and a message pump, which is where they stop being testable --
# so the policy is pure and the affinity seam takes its platform call through a
# function it can be handed a stub for.
exosnap_add_gtest(
    NAME window_presence_tests
    TEST_PREFIX quick_window_presence.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_window_presence.cpp
        MainWindowAffinity.cpp
        MainWindowAffinity.h
        # The chrome is here for one reason: both minimize routes -- the title
        # bar's button and the SC_MINIMIZE Windows sends -- have to be shown
        # consulting the SAME provider, and neither needs a window to reach that
        # decision.
        QuickWindowChrome.cpp
        QuickWindowChrome.h
        QuickWindowGeometry.cpp
        QuickWindowGeometry.h
        ${PROJECT_SOURCE_DIR}/app/ui/WindowGeometryPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/WindowPresencePolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/AppLog.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
)

target_include_directories(window_presence_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(window_presence_tests PRIVATE NOMINMAX)
set_target_properties(window_presence_tests PROPERTIES AUTOMOC ON)

# The shell's own view of the recording session: the projection every shell
# surface reads, the heartbeat they share, and who owns the taskbar progress bar.
# All three are pure -- no COM, no window, no timers -- which is what lets the
# stale-callback and ownership-race cases be written at all.
exosnap_add_gtest(
    NAME shell_presence_tests
    TEST_PREFIX quick_shell_presence.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_shell_presence.cpp
        ${PROJECT_SOURCE_DIR}/app/models/ShellPresence.cpp
        ${PROJECT_SOURCE_DIR}/app/models/ShellPresence.h
        ${PROJECT_SOURCE_DIR}/app/models/RecordingPulse.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingPulse.h
        ${PROJECT_SOURCE_DIR}/app/models/TaskbarProgressLease.cpp
        ${PROJECT_SOURCE_DIR}/app/models/TaskbarProgressLease.h
    LIBRARIES
        Qt6::Core
        exosnap::capability
        exosnap_engine
)

target_include_directories(shell_presence_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(shell_presence_tests PRIVATE NOMINMAX)

# The three surfaces that turn the projection into something a user sees: the
# adapter that owns the entry heartbeat and the Saved dwell, the taskbar button
# with its readiness lifecycle, and the tray's model. The platform calls go
# through an injectable seam, so the lifecycle and every refusal are testable
# without Explorer -- and the tray's model is a plain QObject, so its menu is
# assertable without a notification area.
exosnap_add_gtest(
    NAME shell_surface_tests
    TEST_PREFIX quick_shell_surface.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_shell_surfaces.cpp
        ${EXOSNAP_QUICK_TESTS_DIR}/test_thumb_icon_appearance.cpp
        ShellPresenceAdapter.cpp
        ShellPresenceAdapter.h
        TaskbarPresence.cpp
        TaskbarPresence.h
        TrayAdapter.cpp
        TrayAdapter.h
        ${PROJECT_SOURCE_DIR}/app/models/ShellPresence.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingPulse.cpp
        ${PROJECT_SOURCE_DIR}/app/models/TaskbarProgressLease.cpp
        ${PROJECT_SOURCE_DIR}/app/ui/brand/BrandMarkSvg.cpp
        ${PROJECT_SOURCE_DIR}/app/ui/brand/ShellIconRenderer.cpp
        ${PROJECT_SOURCE_DIR}/app/assets/brand/exosnap_marks.qrc
        ${PROJECT_SOURCE_DIR}/app/diagnostics/AppLog.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Svg
        Qt6::Qml
        Qt6::Test
        exosnap::capability
        exosnap_engine
        # ITaskbarList3 is created through CoCreateInstance, and its CLSID/IID
        # come out of uuid.lib.
        ole32
        uuid
)

set_target_properties(shell_surface_tests PROPERTIES AUTORCC ON)

target_include_directories(shell_surface_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(shell_surface_tests PRIVATE NOMINMAX)
set_target_properties(shell_surface_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME edit_qml_adapter_tests
    TEST_PREFIX quick_edit_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_edit_adapters.cpp
        EditExportAdapter.cpp
        EditExportAdapter.h
        EditSessionAdapter.cpp
        EditSessionAdapter.h
        EditTimelineAdapter.cpp
        EditTimelineAdapter.h
        EditTimelineModels.cpp
        EditTimelineModels.h
        ${PROJECT_SOURCE_DIR}/app/services/TimelineThumbnailSource.cpp
        ${PROJECT_SOURCE_DIR}/app/services/TimelineThumbnailSource.h
    # exosnap_app_log_test_support: loadMarkers() reports markers the sidecar
    # parser had to skip (QCR-207) through AppLog, same as DeviceAdapter does
    # for its rescan milestones.
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        exosnap_app_log_test_support
        exosnap_engine
)

target_include_directories(edit_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(edit_qml_adapter_tests PRIVATE NOMINMAX)

# Startup recovery policy at the QML boundary: what is offered, what a
# destructive action requires before it runs, and what survives a dismissal.
# The repair remux itself stays covered by recovery_drill_tests, which drives it
# against real synthetic-pipeline artefacts.
exosnap_add_gtest(
    NAME recovery_qml_adapter_tests
    TEST_PREFIX quick_recovery_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_recovery_adapter.cpp
        RecoveryAdapter.cpp
        RecoveryAdapter.h
        ${PROJECT_SOURCE_DIR}/app/services/RecoveryService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/AtomicFileOps.cpp
        ${PROJECT_SOURCE_DIR}/app/settings/RecoveryManifestStore.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/AppLog.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Qml
        Qt6::Test
        exosnap_capability
        exosnap_engine
        FFmpeg::mux
)

target_include_directories(recovery_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(recovery_qml_adapter_tests PRIVATE NOMINMAX)

# Which of the two startup modal surfaces may be up. Both real adapters are
# compiled in rather than doubled: "is recovery still open" and "is the crash
# prompt still active" are their own state machines, and a double would let the
# arbitration agree with a model of them instead of with them.
exosnap_add_gtest(
    NAME blocking_surface_arbiter_tests
    TEST_PREFIX quick_blocking_surface.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_blocking_surface_arbiter.cpp
        BlockingSurfaceArbiter.cpp
        BlockingSurfaceArbiter.h
        CrashReportAdapter.cpp
        CrashReportAdapter.h
        RecoveryAdapter.cpp
        RecoveryAdapter.h
        # QCR-415: the third arbitrated surface.
        RecordingErrorAdapter.cpp
        RecordingErrorAdapter.h
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.h
        ${PROJECT_SOURCE_DIR}/app/services/RecoveryService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/AtomicFileOps.cpp
        ${PROJECT_SOURCE_DIR}/app/settings/RecoveryManifestStore.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/AppLog.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Test
        exosnap_capability
        exosnap_engine
        FFmpeg::mux
)

target_compile_definitions(blocking_surface_arbiter_tests PRIVATE NOMINMAX)

target_include_directories(blocking_surface_arbiter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(blocking_surface_arbiter_tests PRIVATE NOMINMAX)

# The overlay gating and the content policy. RecordViewModel is pulled in whole
# because the adapter reads the real view model rather than a test double --
# "which UiRecordingState counts as a live capture" is exactly what the tests
# are checking, and a double would let that answer drift.
exosnap_add_gtest(
    NAME overlay_qml_adapter_tests
    TEST_PREFIX quick_overlay_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_overlay_adapter.cpp
        OverlayAdapter.cpp
        OverlayAdapter.h
        ${PROJECT_SOURCE_DIR}/app/models/OverlayContentPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/OverlayContentPolicy.h
        ${PROJECT_SOURCE_DIR}/app/services/ScreenPresentation.cpp
        ${PROJECT_SOURCE_DIR}/app/services/ScreenPresentation.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/AppLog.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        exosnap_record_viewmodel_test_support
        exosnap::capability
        exosnap_engine
)

target_include_directories(overlay_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(overlay_qml_adapter_tests PRIVATE NOMINMAX)

# Recording-failure policy at the QML boundary: which results raise the surface,
# what it says, which fact rows it shows, and what each action does. The policy
# itself (models/RecordingFailurePolicy) is the single authority the Widgets
# RecordPage reads too, so a divergence here is a divergence there.
exosnap_add_gtest(
    NAME recording_error_qml_adapter_tests
    TEST_PREFIX quick_recording_error_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_recording_error_adapter.cpp
        RecordingErrorAdapter.cpp
        RecordingErrorAdapter.h
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.h
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Test
        exosnap_capability
        exosnap_engine
)

target_include_directories(recording_error_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(recording_error_qml_adapter_tests PRIVATE NOMINMAX)

# The out-of-window toast stack mirrors the manager's visible set rather than
# keeping a second idea of what is on screen, and the single low-storage
# producer maps the disk-guard auto-stop onto the right event. The visibility
# rules themselves stay covered by notification_manager_tests.
exosnap_add_gtest(
    NAME notification_toast_qml_tests
    TEST_PREFIX quick_notification_toast.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_notification_toasts.cpp
        NotificationToastModel.cpp
        NotificationToastModel.h
        NotificationsAdapter.cpp
        NotificationsAdapter.h
        NotificationEntryModel.cpp
        NotificationEntryModel.h
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationManager.cpp
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationNames.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Test
        exosnap_capability
        exosnap_engine
)

target_include_directories(notification_toast_qml_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(notification_toast_qml_tests PRIVATE NOMINMAX)

# The crash-consent surface. Asserts privacy properties, not layout: what each
# click commits, what dismissing deliberately does NOT commit, and that the
# remember tick never leaks between decisions. The decision function itself
# (models/CrashReportPolicy) is shared with the Widgets shell.
exosnap_add_gtest(
    NAME crash_report_qml_adapter_tests
    TEST_PREFIX quick_crash_report_adapter.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_crash_report_adapter.cpp
        CrashReportAdapter.cpp
        CrashReportAdapter.h
    LIBRARIES
        Qt6::Core
        Qt6::Qml
        Qt6::Test
)

target_include_directories(crash_report_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(crash_report_qml_adapter_tests PRIVATE NOMINMAX)
