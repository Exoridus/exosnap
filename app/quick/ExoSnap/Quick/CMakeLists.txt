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
        DiagnosticsSectionHeader.qml
        DiagnosticsSelfTestRow.qml
        EditDetailsRail.qml
        EditExportPanel.qml
        EditOverlay.qml
        EditPlayer.qml
        EditTimeline.qml
        ExoBadge.qml
        ExoBrandMark.qml
        ExoButton.qml
        ExoCard.qml
        ExoCheckBox.qml
        ExoChevron.qml
        ExoConfirmDialog.qml
        ExoDisclosure.qml
        ExoGlyph.qml
        ExoIssueCard.qml
        ExoKeyValueTable.qml
        ExoLogView.qml
        ExoMenu.qml
        ExoMenuItem.qml
        ExoNavTab.qml
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
        ExoSettingRow.qml
        ExoSlider.qml
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
        NotificationToastCard.qml
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
        SettingsAudioSection.qml
        SettingsAudioSourceRow.qml
        SettingsChromaSlider.qml
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
        SettingsPresetNameDialog.qml
        SettingsQualitySection.qml
        SettingsUpdatesSection.qml
        SettingsWebcamSection.qml
        WindowChromeButton.qml
    SOURCES
        AboutViewModelAdapter.cpp
        AboutViewModelAdapter.h
        CaptureExclusion.cpp
        CaptureExclusion.h
        CrashReportAdapter.cpp
        CrashReportAdapter.h
        DeviceAdapter.cpp
        DeviceAdapter.h
        DiagnosticIssueModel.cpp
        DiagnosticIssueModel.h
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
        QuickWindowChrome.cpp
        QuickWindowChrome.h
        QuickWindowGeometry.cpp
        QuickWindowGeometry.h
)

target_sources(exosnap
    PRIVATE
        QuickApplication.cpp
        QuickApplication.h
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
        ${PROJECT_SOURCE_DIR}/app/models/AboutInfo.cpp
        ${PROJECT_SOURCE_DIR}/app/models/AboutInfo.h
        ${PROJECT_SOURCE_DIR}/app/models/RecordingPresetRegistry.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingPresetRegistry.h
        ${PROJECT_SOURCE_DIR}/app/models/EditContext.h
        ${PROJECT_SOURCE_DIR}/app/models/EditContextFactory.cpp
        ${PROJECT_SOURCE_DIR}/app/models/EditContextFactory.h
        ${PROJECT_SOURCE_DIR}/app/models/CloseGuardPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/CloseGuardPolicy.h
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.h
        ${PROJECT_SOURCE_DIR}/app/models/OverlayContentPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/models/OverlayContentPolicy.h
        ${PROJECT_SOURCE_DIR}/app/models/HotkeyStartupConflicts.h
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationEvent.h
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationManager.cpp
        ${PROJECT_SOURCE_DIR}/app/notifications/NotificationManager.h
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
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsController.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsController.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsProbe.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsProbe.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/FixActionDispatcher.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/FixActionDispatcher.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/LogViewPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/LogViewPolicy.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/CrashSessionContext.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/CrashSessionContext.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/ElevationProvider.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/SupportBundle.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/ZipWriter.cpp
        ${PROJECT_SOURCE_DIR}/app/services/SupportBundleService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/SupportBundleService.h
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
        ${PROJECT_SOURCE_DIR}/app/services/RecordingCoordinator.cpp
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
        ${PROJECT_SOURCE_DIR}/app/viewmodels/RecordViewModel.cpp
        ${PROJECT_SOURCE_DIR}/app/viewmodels/RecordViewModel.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/error_message.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/error_message.h
        # Toolkit-free geometry policy (QRect/QSize only) despite its ui/ home;
        # the Quick window restore reuses it unchanged rather than reimplementing
        # the clamp rules a second time.
        ${PROJECT_SOURCE_DIR}/app/ui/WindowGeometryPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/ui/WindowGeometryPolicy.h
        # The one genuinely Qt Widgets-bound file this frontend compiles, and the
        # same one the Widgets target builds -- not a Quick-flavoured copy. Qt has
        # no Quick/QML system-tray type, so QSystemTrayIcon (and with it
        # QApplication) is the documented intentional remainder of Qt6::Widgets in
        # the shipped Quick application. See the cutover plan, "Intentional
        # remaining Widgets dependency".
        ${PROJECT_SOURCE_DIR}/app/ui/tray/TrayPresence.cpp
        ${PROJECT_SOURCE_DIR}/app/ui/tray/TrayPresence.h
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

target_link_libraries(exosnap
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
        # Solely for QSystemTrayIcon (via ui/tray/TrayPresence). Nothing this
        # application draws is a QWidget. Do not grow this dependency: any other
        # Widgets type appearing here means a Quick equivalent was skipped.
        Qt6::Widgets
        exosnap::warnings
        exosnap::capability
        exosnap::update
        exosnap::crash_capture
        exosnap_output_model_test_support
        exosnap_device_service_test_support
        exosnap_diagnostics_test_support
        exosnap_live_verify
        exosnap_miniz
        recorder_core
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

# recorder_core links the FFmpeg-backed recording path even though this slice
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
    install(SCRIPT ${_exosnap_deploy_script})
endif()

add_test(NAME quick.qml.about_smoke COMMAND exosnap --smoke-test)
set_tests_properties(quick.qml.about_smoke PROPERTIES LABELS "quick")

qt_add_executable(record_controls_qml_tests
    ${EXOSNAP_QUICK_TESTS_DIR}/qml_test_main.cpp
    ${PROJECT_SOURCE_DIR}/app/assets/fonts/exosnap_fonts.qrc
)

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
    QML_FILES
        ExoButton.qml
        ExoChevron.qml
        ExoDisclosure.qml
        # The record transport's two controls draw their icons with ExoGlyph, so
        # the test module has to carry it as well — without it qmllint resolves
        # nothing inside either component and reports the whole icon block.
        ExoGlyph.qml
        ExoSegmentedControl.qml
        ExoTheme.qml
        PreviewMetricsOverlay.qml
        RecordActionButton.qml
        RecordSourceToggle.qml
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

add_test(NAME quick.qml.record_preview_smoke COMMAND exosnap --preview-smoke-test)
set_tests_properties(quick.qml.record_preview_smoke PROPERTIES
    LABELS "quick;live"
    ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:windows"
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 3
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
    LIBRARIES
        Qt6::Core
        Qt6::Quick
        exosnap_record_viewmodel_test_support
        exosnap::capability
        recorder_core
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
        recorder_core
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
        recorder_core
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
        recorder_core
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

# Diagnostics/Logs presentation policy extracted out of the Widgets DiagnosticsPage,
# LogsPage and MainWindow. Registered here rather than in app/CMakeLists.txt so the
# Phase-B migration keeps its shared-file footprint to the Quick module.
exosnap_add_gtest(
    NAME diagnostics_policy_tests
    TEST_PREFIX quick_diagnostics_policy.
    SOURCES
        ${EXOSNAP_QUICK_TESTS_DIR}/test_diagnostics_policy.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsController.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsProbe.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/FixActionDispatcher.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        exosnap_diagnostics_test_support
        exosnap::capability
        recorder_core
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
        DiagnosticIssueModel.cpp
        DiagnosticIssueModel.h
        DiagnosticsAdapter.cpp
        DiagnosticsAdapter.h
        LogEntryModel.cpp
        LogEntryModel.h
        LogsAdapter.cpp
        LogsAdapter.h
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsController.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/DiagnosticsProbe.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/FixActionDispatcher.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/LogViewPolicy.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/ElevationProvider.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/SupportBundle.cpp
        ${PROJECT_SOURCE_DIR}/app/diagnostics/ZipWriter.cpp
        ${PROJECT_SOURCE_DIR}/app/services/SupportBundleService.cpp
        ${PROJECT_SOURCE_DIR}/app/services/SupportBundleService.h
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        exosnap_app_log_test_support
        exosnap_diagnostics_test_support
        exosnap_miniz
        exosnap::capability
        exosnap::update
        recorder_core
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
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        recorder_core
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
        recorder_core
        FFmpeg::mux
)

target_include_directories(recovery_qml_adapter_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/app
)

target_compile_definitions(recovery_qml_adapter_tests PRIVATE NOMINMAX)

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
        recorder_core
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
        recorder_core
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
        ${PROJECT_SOURCE_DIR}/app/models/RecordingFailurePolicy.cpp
    LIBRARIES
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Test
        exosnap_capability
        recorder_core
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
