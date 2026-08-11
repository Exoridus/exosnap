cmake_minimum_required(VERSION 3.27)

#Local Qt install hint — override via Qt6_DIR or CMAKE_PREFIX_PATH.
if(NOT Qt6_DIR)
    list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.9.0/msvc2022_64")
endif()

# Whether to build the ExoSnap Qt application at all. Defaulted ON; flip OFF for
# configurations that only need the core engine libraries and tests (headless
# CI, contributors without Qt installed). Kept as an explicit option rather than
# leaving "should we build the app" implied by Qt's presence alone.
option(EXOSNAP_BUILD_APP "Build the ExoSnap Qt application" ON)
option(EXOSNAP_BUILD_QUICK_SPIKE "Build the isolated Qt Quick frontend spike" OFF)

# The --auto-record / --visual-test automation harnesses are Debug-only by
# default: they have no shipping caller, so a normal Release must not carry
# them. That rule breaks down for exactly one job — comparing the production
# cost of the two frontends. A Debug measurement compares two unoptimised
# builds and says nothing about what users run.
#
# Turning this ON adds the harness code to a genuine Release build and changes
# NOTHING else: no optimisation flag, no runtime policy, no shipping default.
# The delta between `Release` and `Release + this option` is the deliberately
# included automation code and nothing besides. Never enable it for a shipped
# artifact.
option(EXOSNAP_BUILD_BENCHMARK_HARNESS
       "Include the --auto-record benchmark/automation harness in Release builds" OFF)

# Gate used by every harness source and compile definition below. Written as a
# generator expression because the source lists are per-config: `Debug` keeps
# the harness implicitly, `Release` only gets it when the option is on.
if(EXOSNAP_BUILD_BENCHMARK_HARNESS)
    set(EXOSNAP_HARNESS_GATE "1")
else()
    set(EXOSNAP_HARNESS_GATE "$<NOT:$<CONFIG:Release>>")
endif()

# Always probe for Qt6, even when EXOSNAP_BUILD_APP is OFF: this also caches
# Qt6_DIR, which apps/updater/CMakeLists.txt relies on to find Qt6 in its own
# (sibling, not nested) directory scope without repeating the CMAKE_PREFIX_PATH
# hint above. Skipping this call when the app build is disabled would silently
# break Qt detection for the updater even on a machine where Qt is installed.
find_package(Qt6 6.9 QUIET COMPONENTS Core Gui Widgets Svg)

# A stand-in `exosnap` custom target is created in every branch below other than
# the real build, so add_subdirectory(app) always leaves a target named `exosnap`
# behind for anything downstream that references it defensively (see
# apps/updater/CMakeLists.txt and tests/CMakeLists.txt). Nothing must call
# target_link_libraries()/target_compile_definitions() on this stand-in — those
# only ever run below, after the real qt_add_executable(exosnap ...) target
# exists.
if(NOT EXOSNAP_BUILD_APP OR NOT Qt6_FOUND)
    if(EXOSNAP_BUILD_APP)
        add_custom_target(exosnap COMMAND ${CMAKE_COMMAND} -E echo "exosnap skipped: Qt6 not found")
        message(WARNING "Qt6 not found — exosnap app will not be built. Install Qt 6 and set Qt6_DIR or CMAKE_PREFIX_PATH.")
    else()
        add_custom_target(exosnap COMMAND ${CMAKE_COMMAND} -E echo "exosnap skipped: EXOSNAP_BUILD_APP is OFF")
        message(STATUS "EXOSNAP_BUILD_APP is OFF — exosnap app will not be built.")
    endif()
    return()
endif()

qt_standard_project_setup(REQUIRES 6.9)

if(EXOSNAP_BUILD_QUICK_SPIKE)
    # Test is not used directly, but Qt6QuickTest links it, and the test-DLL
    # staging in cmake/exosnap_testing.cmake can only copy a module whose
    # imported target actually exists. Without it a QuickTest binary builds and
    # then fails to START with 0xC0000135.
    find_package(Qt6 6.9 REQUIRED COMPONENTS Qml Quick QuickControls2 QuickDialogs2 QuickTest Test)
endif()

# Qt test executables need Qt DLLs on PATH during gtest_discover_tests (POST_BUILD)
# and during CTest execution. Add Qt's bin directory to the environment so both work.
get_target_property(_qt6_core_loc Qt6::Core IMPORTED_LOCATION)
if(NOT _qt6_core_loc)
    get_target_property(_qt6_core_loc Qt6::Core LOCATION)
endif()
if(_qt6_core_loc)
    get_filename_component(_qt6_bin_dir "${_qt6_core_loc}" DIRECTORY)
    if(WIN32 AND _qt6_bin_dir)
        string(REGEX REPLACE "/$" "" _qt6_bin_dir "${_qt6_bin_dir}")
        set(ENV{PATH} "${_qt6_bin_dir}\;$ENV{PATH}")
    endif()
endif()

# ---- Build metadata ----
# Single source of truth: the root project(... VERSION ...) call plus the
# release identity resolved into EXOSNAP_FULL_VERSION (root CMakeLists.txt).
# Never hard-code a version string here. EXOSNAP_VERSION carries the FULL
# release version (e.g. "0.9.0-rc4", dev builds "0.9.0-dev") and feeds the
# runtime kVersion and the VERSIONINFO ProductVersion string; the numeric
# fields below stay base-only because VERSIONINFO is numeric by design.
set(EXOSNAP_VERSION "${EXOSNAP_FULL_VERSION}")

# Windows VERSIONINFO requires a four-field numeric version. CMake leaves
# PROJECT_VERSION_TWEAK empty for an X.Y.Z version, so default the tweak to 0.
set(_exosnap_ver_tweak "${PROJECT_VERSION_TWEAK}")
if(_exosnap_ver_tweak STREQUAL "")
    set(_exosnap_ver_tweak 0)
endif()
set(EXOSNAP_VERSION_COMMA
    "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},${_exosnap_ver_tweak}")
set(EXOSNAP_VERSION_FILE_STR
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${_exosnap_ver_tweak}")

find_package(Git QUIET)

if(EXOSNAP_OFFICIAL_BUILD)
    set(_exosnap_official_flag 1)
else()
    set(_exosnap_official_flag 0)
endif()

set(EXOSNAP_BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/ExoSnapBuildInfo.h")
add_custom_target(exosnap_build_info
    ALL
    COMMAND
        ${CMAKE_COMMAND}
        -DEXOSNAP_VERSION=${EXOSNAP_VERSION}
        -DEXOSNAP_BASE_VERSION=${PROJECT_VERSION}
        -DEXOSNAP_OFFICIAL=${_exosnap_official_flag}
        -DEXOSNAP_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DEXOSNAP_BUILD_INFO_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/ExoSnapBuildInfo.h.in
        -DEXOSNAP_BUILD_INFO_OUTPUT=${EXOSNAP_BUILD_INFO_HEADER}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/GenerateBuildInfo.cmake
    BYPRODUCTS "${EXOSNAP_BUILD_INFO_HEADER}"
    COMMENT "Refreshing ExoSnap build metadata"
    VERBATIM
)

qt_add_executable(exosnap WIN32
    main.cpp
    startup_log.h
    startup_log.cpp
    assets/brand/exosnap_brand.qrc
    assets/fonts/exosnap_fonts.qrc
    ui/theme/ExoSnapPalette.h
    ui/theme/ExoSnapMetrics.h
    ui/theme/ExoSnapTheme.h
    ui/theme/ExoSnapTheme.cpp
    ui/theme/ExoSnapThemes.h
    ui/theme/exosnap_theme.qrc
    ui/theme/LucideIcon.h
    ui/theme/LucideIcon.cpp
    ui/brand/BrandMarkWidget.h
    ui/brand/BrandMarkWidget.cpp
    ui/WindowGeometryPolicy.h
    ui/WindowGeometryPolicy.cpp
    ui/chrome/NotificationHubPanel.h
    ui/chrome/NotificationHubPanel.cpp
    ui/chrome/OperationalTitleBar.h
    ui/chrome/OperationalTitleBar.cpp
    ui/chrome/GlobalRecordingBar.h
    ui/chrome/GlobalRecordingBar.cpp
    ui/dialogs/RecoveryOverlay.h
    ui/dialogs/RecoveryOverlay.cpp
    ui/dialogs/WhatsNewOverlay.h
    ui/dialogs/WhatsNewOverlay.cpp
    ui/dialogs/SourcePickerPanel.h
    ui/dialogs/SourcePickerPanel.cpp
    ui/dialogs/SourcePickerDialog.h
    ui/dialogs/SourcePickerDialog.cpp
    ui/dialogs/SourcePickerOverlay.h
    ui/dialogs/SourcePickerOverlay.cpp
    ui/dialogs/SourcePickerWindowRules.h
    ui/dialogs/EditExportOverlay.h
    ui/dialogs/EditExportOverlay.cpp
    ui/dialogs/FinalizingOverlay.h
    ui/dialogs/FinalizingOverlay.cpp
    ui/dialogs/CrashReportPanel.h
    ui/dialogs/CrashReportPanel.cpp
    ui/dialogs/CrashReportOverlay.h
    ui/dialogs/CrashReportOverlay.cpp
    ui/dialogs/RecordingErrorPanel.h
    ui/dialogs/RecordingErrorPanel.cpp
    ui/dialogs/RecordingErrorOverlay.h
    ui/dialogs/RecordingErrorOverlay.cpp
    ui/tray/TrayPresence.h
    ui/tray/TrayPresence.cpp
    ui/widgets/SectionRuleHeader.h
    ui/widgets/SectionRuleHeader.cpp
    ui/widgets/KeycapChip.h
    ui/widgets/KeycapChip.cpp
    ui/widgets/HotkeysSettingsPanel.h
    ui/widgets/HotkeysSettingsPanel.cpp
    ui/widgets/StatusPill.h
    ui/widgets/StatusPill.cpp
    ui/widgets/PreviewSurface.h
    ui/widgets/PreviewSurface.cpp
    ui/widgets/CameraPreview.h
    ui/widgets/CameraPreview.cpp
    ui/widgets/EditPlayerSurface.h
    ui/widgets/EditPlayerSurface.cpp
    ui/widgets/EditDetailsRail.h
    ui/widgets/EditDetailsRail.cpp
    ui/widgets/ExportPanel.h
    ui/widgets/ExportPanel.cpp
    ui/widgets/WebcamSetupPanel.h
    ui/widgets/WebcamSetupPanel.cpp
    ui/widgets/CaptureTargetCard.h
    ui/widgets/CaptureTargetCard.cpp
    ui/widgets/RegionPresetCard.h
    ui/widgets/RegionPresetCard.cpp
    ui/widgets/DeviceAdapterCard.h
    ui/widgets/DeviceAdapterCard.cpp
    ui/widgets/PipelineStepCard.h
    ui/widgets/PipelineStepCard.cpp
    ui/widgets/PipelineFlow.h
    ui/widgets/PipelineFlow.cpp
    ui/widgets/LivePipelinePanel.h
    ui/widgets/LivePipelinePanel.cpp
    ui/widgets/TipChip.h
    ui/widgets/TipChip.cpp
    ui/widgets/ElevationLock.h
    ui/widgets/ElevationLock.cpp
    ui/widgets/CodecCard.h
    ui/widgets/CodecCard.cpp
    ui/widgets/VUMeterWidget.h
    ui/widgets/VUMeterWidget.cpp
    ui/widgets/ExoToggle.h
    ui/widgets/ExoToggle.cpp
    ui/widgets/ExoCheckBox.h
    ui/widgets/ExoCheckBox.cpp
    ui/widgets/InfoHintIcon.h
    ui/widgets/InfoHintIcon.cpp
    ui/widgets/CompareHint.h
    ui/widgets/CompareHint.cpp
    ui/widgets/SettingsCardExpander.h
    ui/widgets/SettingsCardExpander.cpp
    ui/widgets/ComboBoxWheelFilter.h
    ui/widgets/ComboBoxWheelFilter.cpp
    ui/widgets/RegionSelectionOverlay.h
    ui/widgets/RegionSelectionOverlay.cpp
    ui/widgets/RegionGeometry.h
    ui/widgets/RegionGeometry.cpp
    ui/widgets/AudioSourceRow.h
    ui/widgets/AudioSourceRow.cpp
    ui/widgets/AudioSourceToggle.h
    ui/widgets/AudioSourceToggle.cpp
    ui/widgets/ExoSpinBox.h
    ui/widgets/ExoSpinBox.cpp
    ui/widgets/ExoSlider.h
    ui/widgets/ExoSlider.cpp
    ui/widgets/NotificationBell.h
    ui/widgets/NotificationBell.cpp
    ui/widgets/AdvisoryItem.h
    ui/widgets/AdvisoryItem.cpp
    ui/CodecLabels.h
    ui/widgets/CornerCaptureButton.h
    ui/widgets/CornerCaptureButton.cpp
    ui/widgets/PlaceholderRow.h
    ui/widgets/PlaceholderRow.cpp
    ui/widgets/TransportDock.h
    ui/widgets/TransportDock.cpp
    ui/widgets/EditTimeline.h
    ui/widgets/EditTimeline.cpp
    ui/overlay/CountdownOverlayWindow.h
    ui/overlay/CountdownOverlayWindow.cpp
    ui/overlay/DiagnosticsOverlayWindow.h
    ui/overlay/DiagnosticsOverlayWindow.cpp
    ui/overlay/QuickControlPillWindow.h
    ui/overlay/QuickControlPillWindow.cpp
    ui/overlay/RecordingOverlayWindow.h
    ui/overlay/RecordingOverlayWindow.cpp
    ui/overlay/NotificationToastWindow.h
    ui/overlay/NotificationToastWindow.cpp
    notifications/NotificationEvent.h
    notifications/NotificationManager.h
    notifications/NotificationManager.cpp
    MainWindow.h
    MainWindow.cpp
    pages/AboutPage.h
    pages/AboutPage.cpp
    pages/DevicePage.h
    pages/DevicePage.cpp
    pages/RecordPage.h
    pages/RecordPage.cpp
    pages/ConfigPage.h
    pages/ConfigPage.cpp
    pages/FrameRateLimits.h
    pages/DiagnosticsPage.h
    pages/DiagnosticsPage.cpp
    pages/LogsPage.h
    pages/LogsPage.cpp
    pages/EditExportPage.h
    pages/EditExportPage.cpp
    services/DeviceDiscoveryCommon.h
    services/AudioDeviceNotifier.h
    services/AudioDeviceNotifier.cpp
    services/WebcamDeviceNotifier.h
    services/WebcamDeviceNotifier.cpp
    services/DisplayDeviceNotifier.h
    services/DisplayDeviceNotifier.cpp
    services/CaptureTargetNotifier.h
    services/CaptureTargetNotifier.cpp
    services/GlobalHotkeyService.h
    services/GlobalHotkeyService.cpp
    services/PreviewService.h
    services/PreviewService.cpp
    services/PreviewHelpers.h
    services/CaptureSourceHub.h
    services/CaptureSourceHub.cpp
    services/CaptureSourceKey.h
    services/ThumbnailMip.h
    services/CaptureHubRegistry.h
    services/CaptureHubRegistry.cpp
    services/WgcSourceProducer.h
    services/WgcSourceProducer.cpp
    services/DxgiSourceProducer.h
    services/DxgiSourceProducer.cpp
    services/DxgiCaptureHubService.h
    services/DxgiCaptureHubService.cpp
    services/ThumbnailCapture.h
    services/ThumbnailCapture.cpp
    services/WindowEvidenceProbe.h
    services/WindowEvidenceProbe.cpp
    services/DxgiPreviewRenderer.h
    services/DxgiPreviewRenderer.cpp
    services/EditPlayerRenderer.h
    services/EditPlayerRenderer.cpp
    services/WebcamService.h
    services/WebcamService.cpp
    services/RecordingCoordinator.h
    services/RecordingCoordinator.cpp
    services/DisplayNumbering.h
    services/DisplayNumbering.cpp
    services/ScreenPresentation.h
    services/ScreenPresentation.cpp
    services/TargetDisplayFacts.h
    services/DisplayIdentityResolver.h
    services/DisplayIdentityResolver.cpp
    services/DisplayIdentityEnumerator.h
    services/DisplayIdentityEnumerator.cpp
    services/RecordingCountdownController.h
    services/RecordingCountdownController.cpp
    services/PageHydrationController.h
    services/PageHydrationController.cpp
    services/TimelineThumbnailSource.h
    services/TimelineThumbnailSource.cpp
    services/RecoveryService.h
    services/RecoveryService.cpp
    services/AtomicFileOps.h
    services/AtomicFileOps.cpp
    services/UpdateService.h
    services/UpdateService.cpp
    services/UpdateLaunchPlan.cpp
    services/VerifyReinstallMode.h
    services/VerifyReinstallMode.cpp
    services/WhatsNewPayload.h
    services/WhatsNewPayload.cpp
    services/ElevatedRelaunch.h
    services/ElevatedRelaunch.cpp
    models/WebcamSettings.h
    models/AboutInfo.h
    models/AboutInfo.cpp
    models/SettingsCompareData.h
    models/SettingsCompareData.cpp
    models/OutputSettingsModel.h
    models/OutputSettingsModel.cpp
    models/StableDisplayId.h
    models/RecordingPreset.h
    models/RecordingPreset.cpp
    models/RecordingPresetRegistry.h
    models/RecordingPresetRegistry.cpp
    models/VideoSettingsModel.h
    models/FilenameBuilder.h
    models/FilenameBuilder.cpp
    models/OutputPathValidator.h
    models/OutputPathValidator.cpp
    models/OutputPathPolicy.h
    models/OutputPathPolicy.cpp
    models/EditContextFactory.h
    models/EditContextFactory.cpp
    models/CloseGuardPolicy.h
    models/CloseGuardPolicy.cpp
    models/RecordingFailurePolicy.h
    models/RecordingFailurePolicy.cpp
    models/OverlayContentPolicy.h
    models/OverlayContentPolicy.cpp
    models/HotkeyStartupConflicts.h
    settings/AppSettingsStore.h
    settings/AppSettingsStore.cpp
    settings/RecordingPresetStore.h
    settings/RecordingPresetStore.cpp
    settings/RecordingHistoryStore.h
    settings/RecordingHistoryStore.cpp
    settings/RecoveryManifestStore.h
    settings/RecoveryManifestStore.cpp
    settings/CapabilityCacheStore.h
    settings/CapabilityCacheStore.cpp
    diagnostics/error_message.h
    diagnostics/error_message.cpp
    diagnostics/AppLog.h
    diagnostics/AppLog.cpp
    diagnostics/EngineLogBridge.h
    diagnostics/EngineLogBridge.cpp
    diagnostics/StructuredLog.h
    diagnostics/StructuredLog.cpp
    diagnostics/SessionReport.h
    diagnostics/SessionReport.cpp
    diagnostics/ZipWriter.h
    diagnostics/ZipWriter.cpp
    diagnostics/SupportBundle.h
    diagnostics/SupportBundle.cpp
    diagnostics/DiagnosticResult.h
    diagnostics/DiagnosticResult.cpp
    diagnostics/CapabilitySummary.h
    diagnostics/CapabilitySummary.cpp
    diagnostics/ConfigSummary.h
    diagnostics/ConfigSummary.cpp
    diagnostics/CrashSessionContext.h
    diagnostics/CrashSessionContext.cpp
    diagnostics/DiskSpaceProvider.h
    diagnostics/DiskSpaceProvider.cpp
    diagnostics/ElevationProvider.h
    diagnostics/ElevationProvider.cpp
    diagnostics/PresentProvider.h
    diagnostics/PresentAccumulator.h
    diagnostics/PresentModeMapping.h
    diagnostics/PresentModeMapping.cpp
    diagnostics/PresentMonEtwSession.h
    diagnostics/PresentMonEtwSession.cpp
    diagnostics/PresentMonProvider.h
    diagnostics/PresentMonProvider.cpp
    diagnostics/DpcLatencyProvider.h
    diagnostics/DpcLatencyProvider.cpp
    diagnostics/DiskSpaceThresholds.h
    diagnostics/FilesystemProvider.h
    diagnostics/FilesystemProvider.cpp
    diagnostics/RecommendationEngine.h
    diagnostics/RecommendationEngine.cpp
    diagnostics/WindowTargetFacts.h
    diagnostics/WindowTargetFacts.cpp
    diagnostics/WindowEvidenceAccumulator.h
    diagnostics/WindowEvidenceAccumulator.cpp
    diagnostics/WindowCaptureStall.h
    diagnostics/WindowCaptureStall.cpp
    diagnostics/SelfTestRunner.h
    diagnostics/SelfTestRunner.cpp
    viewmodels/RecordViewModel.h
    viewmodels/RecordViewModel.cpp
    viewmodels/PresentationState.h
    viewmodels/PresentationStateBuilder.h
    viewmodels/PresentationStateBuilder.cpp
)

target_sources(exosnap PRIVATE
    # EXOSNAP_HARNESS_GATE, not a bare non-Release check: the frontend A/B
    # benchmark drives --auto-record --enable-preview, which lives in
    # AutoRecordHarness.cpp behind EXOSNAP_ENABLE_VISUAL_TEST_HARNESS and reaches
    # MainWindow's scenario members. The whole automation cluster therefore has
    # to appear together, or a Release-with-harness build links against a
    # declared-but-undefined preview entry point.
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/visual_tests/VisualScenario.h>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/visual_tests/VisualScenario.cpp>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/visual_tests/VisualTestHarness.h>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/visual_tests/VisualTestHarness.cpp>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/auto_record/AutoRecordHarness.h>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/auto_record/AutoRecordHarness.cpp>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/auto_record/AutoRecordOptions.cpp>"
    # Frontend-neutral benchmark report contract, shared with the Qt Quick target.
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/benchmark/BenchmarkMetrics.h>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/benchmark/BenchmarkReport.h>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/benchmark/BenchmarkReport.cpp>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/benchmark/BenchmarkEngineMetrics.h>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/benchmark/BenchmarkEngineMetrics.cpp>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/benchmark/BenchmarkEffectiveConfig.h>"
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/benchmark/BenchmarkEffectiveConfig.cpp>"
    # MainWindow's visual-test scenario members, split out of MainWindow.cpp.
    # Same gate, matching the guards inside the file and the ones its
    # declarations already carry in MainWindow.h.
    "$<${EXOSNAP_HARNESS_GATE}:${CMAKE_CURRENT_SOURCE_DIR}/MainWindow_VisualScenarios.cpp>"
    # The HWND audit is a standalone harness with no --auto-record caller, so it
    # stays Debug-only and keeps the Release-with-harness delta smaller.
    "$<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}/hwnd_audit/HwndAuditReport.h>"
    "$<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}/hwnd_audit/HwndAuditReport.cpp>"
    "$<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}/hwnd_audit/HwndAuditHarness.h>"
    "$<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}/hwnd_audit/HwndAuditHarness.cpp>"
)

if(WIN32)
    # Windows VERSIONINFO resource — generated from the canonical project version
    # so the installed exosnap.exe carries coherent File/Product version metadata.
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/exosnap_version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/exosnap_version.rc"
        @ONLY
    )
    target_sources(exosnap PRIVATE
        exosnap.rc
        "${CMAKE_CURRENT_BINARY_DIR}/exosnap_version.rc"
    )
endif()

set_target_properties(exosnap PROPERTIES AUTOMOC ON AUTORCC ON OUTPUT_NAME "exosnap")
# EXOSNAP_ENABLE_VISUAL_TEST_HARNESS and EXOSNAP_ENABLE_AUTO_RECORD_HARNESS are relied upon to
# always be defined together: AutoRecordHarness.h unconditionally declares the preview-mode
# RunAutoRecord(MainWindow&) overload, but its definition is gated on
# EXOSNAP_ENABLE_VISUAL_TEST_HARNESS while main.cpp calls it under EXOSNAP_ENABLE_AUTO_RECORD_HARNESS
# — decoupling the two in the future would produce a non-obvious link error.
target_compile_definitions(exosnap PRIVATE
    NOMINMAX
    EXOSNAP_BUILD_CONFIG="$<CONFIG>"
    "$<${EXOSNAP_HARNESS_GATE}:EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1>"
    "$<${EXOSNAP_HARNESS_GATE}:EXOSNAP_ENABLE_AUTO_RECORD_HARNESS=1>"
    "$<$<NOT:$<CONFIG:Release>>:EXOSNAP_ENABLE_HWND_AUDIT_HARNESS=1>"
)
add_dependencies(exosnap exosnap_build_info)

target_link_libraries(exosnap PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
    recorder_core exosnap::capability exosnap::warnings FFmpeg::mux exosnap::crash_capture
    exosnap::update exosnap_miniz
    tomlplusplus::tomlplusplus
    d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib Version.lib
    mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib
    # ELEVATION-FOUNDATION-R1 (ADR 0033): ShellExecuteExW (Shell32) + token APIs
    # OpenProcessToken/GetTokenInformation (Advapi32).
    Shell32.lib Advapi32.lib
)

# ADR 0033: present & tearing diagnostics (PresentMon ETW). EXOSNAP_WITH_PRESENTMON
# and the PresentMon::consumer target are declared at the root level (the option
# before add_subdirectory(third_party), the target inside it), so both are already
# visible by the time this subdirectory runs. Linked here — after the real Qt
# executable target exists — rather than at the root after add_subdirectory(app),
# so it never runs against the custom-target stand-in used when Qt6 is not found
# or EXOSNAP_BUILD_APP is OFF.
if(EXOSNAP_WITH_PRESENTMON)
    target_link_libraries(exosnap PRIVATE PresentMon::consumer)
    target_compile_definitions(exosnap PRIVATE EXOSNAP_HAS_PRESENTMON=1)
endif()

# S4: Delay-load Media Foundation DLLs so the app runs on Windows-N without the
# Media Feature Pack. /DELAYLOAD converts the static import-table entry to a
# demand-load binding: mfplat.dll / mf.dll / mfreadwrite.dll are NOT loaded at
# process startup. WebcamService::IsMfPresent() probes availability via a safe
# LoadLibraryW call before any MF entry point is invoked, so the delay-load
# failure hook (__delayLoadHelper2 raising VcppException) is never reached.
# delayimp.lib provides __delayLoadHelper2 (required by the MSVC delay-load stub).
if(MSVC)
    target_link_options(exosnap PRIVATE
        /DELAYLOAD:mfplat.dll
        /DELAYLOAD:mf.dll
        /DELAYLOAD:mfreadwrite.dll
    )
    target_link_libraries(exosnap PRIVATE delayimp.lib)
endif()

target_include_directories(exosnap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

# Stage crashpad_handler.exe next to exosnap.exe so the runtime lookup
# (crash_capture::ResolveHandlerExePath) resolves it when running from the build
# tree. Only relevant when crash capture is enabled (sentry-native + Crashpad
# fetched); inert in the default build. Resolve the handler via the
# crashpad_handler TARGET (generator expression) rather than a fixed path: the
# Visual Studio multi-config generator emits the exe under a per-config subdir
# (Debug/, Release/) that the cached EXOSNAP_CRASHPAD_HANDLER_EXE string misses.
# The cached path stays the source of truth for the install() rule in VendorSentry.
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

# ---- Dev convenience: stage the Qt runtime next to the build-tree exe ----
# The build-tree exosnap.exe (and every test .exe — they share app/<config>/)
# links against Qt DLLs that live in Qt's bin dir, not next to the exe. Without
# them on PATH, launching raises one Windows error dialog per missing DLL and
# tests abort with 0xC0000135 (STATUS_DLL_NOT_FOUND). windeployqt copies the Qt
# runtime + required plugins (platforms/styles/imageformats) next to the exe so
# launching and ctest always work with no PATH juggling. Release packaging
# deploys separately; this is build-tree only, and POST_BUILD only fires when
# exosnap is actually (re)linked.
if(WIN32)
    find_program(EXOSNAP_WINDEPLOYQT windeployqt HINTS "${_qt6_bin_dir}")
    if(EXOSNAP_WINDEPLOYQT)
        add_custom_command(TARGET exosnap POST_BUILD
            COMMAND "${EXOSNAP_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                "$<IF:$<CONFIG:Debug>,--debug,--release>" "$<TARGET_FILE:exosnap>"
            COMMENT "Staging Qt runtime next to exosnap.exe (build tree)"
            VERBATIM
        )
    else()
        message(STATUS "windeployqt not found — build-tree exe will rely on Qt being on PATH.")
    endif()

    # Deploy the FFmpeg runtime DLLs next to the exe so the build-tree exosnap.exe
    # launches in BOTH Debug and Release. Previously these only landed in the Debug
    # tree incidentally (the gtest targets copy them next to their binaries, which
    # share app/Debug/); a Release build of just the exosnap target left them out,
    # so launching app/Release/exosnap.exe aborted with 0xC0000135
    # (STATUS_DLL_NOT_FOUND) — the app links FFmpeg::mux for MP4 remux and the audio
    # resampler. Release packaging installs these separately (install(FILES …) in
    # VendorFFmpeg.cmake); this is the build-tree equivalent of the Qt deploy above.
    foreach(_exosnap_ffmpeg_dll IN LISTS EXOSNAP_FFMPEG_DLLS)
        add_custom_command(TARGET exosnap POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_exosnap_ffmpeg_dll}" "$<TARGET_FILE_DIR:exosnap>"
            COMMENT "Staging FFmpeg runtime next to exosnap.exe (build tree)"
            VERBATIM
        )
    endforeach()

    # No-op unless EXOSNAP_ASAN=ON — same 0xC0000135 reasoning as the two blocks
    # above, for the sanitizer runtime that ships next to cl.exe.
    exosnap_stage_asan_runtime(exosnap)
endif()

function(exosnap_add_app_test_support NAME)
    add_library(${NAME} STATIC ${ARGN})
    target_include_directories(${NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(${NAME} PUBLIC NOMINMAX)
    target_link_libraries(${NAME} PUBLIC exosnap::warnings)
    set_target_properties(${NAME} PROPERTIES AUTOMOC ON)
endfunction()

exosnap_add_app_test_support(exosnap_app_log_test_support
    diagnostics/AppLog.cpp
)
target_link_libraries(exosnap_app_log_test_support PUBLIC Qt6::Core)

exosnap_add_app_test_support(exosnap_output_model_test_support
    models/FilenameBuilder.cpp
    models/OutputPathPolicy.cpp
    models/OutputPathValidator.cpp
    models/OutputSettingsModel.cpp
    models/RecordingPreset.cpp
)
target_link_libraries(exosnap_output_model_test_support PUBLIC
    Qt6::Core Qt6::Gui exosnap_capability recorder_core ole32.lib shell32.lib
)

exosnap_add_app_test_support(exosnap_record_viewmodel_test_support
    viewmodels/RecordViewModel.cpp
    diagnostics/error_message.cpp
    settings/RecordingHistoryStore.cpp
)
target_link_libraries(exosnap_record_viewmodel_test_support PUBLIC
    exosnap_app_log_test_support exosnap_output_model_test_support Qt6::Core Qt6::Gui Qt6::Widgets
)

exosnap_add_app_test_support(exosnap_diagnostics_test_support
    diagnostics/DiagnosticResult.cpp
    diagnostics/CapabilitySummary.cpp
    diagnostics/ConfigSummary.cpp
    diagnostics/DiskSpaceProvider.cpp
    diagnostics/FilesystemProvider.cpp
    diagnostics/RecommendationEngine.cpp
    diagnostics/WindowTargetFacts.cpp
    diagnostics/SelfTestRunner.cpp
    # Session report writer is called by RecordingCoordinator, which is compiled
    # into the coordinator test targets that all link this support lib.
    diagnostics/SessionReport.cpp
)
target_link_libraries(exosnap_diagnostics_test_support PUBLIC
    exosnap_output_model_test_support exosnap_capability recorder_core
    exosnap::crash_capture
    dxgi.lib mmdevapi.lib dwmapi.lib shell32.lib
)

exosnap_add_app_test_support(exosnap_device_service_test_support
    services/AudioDeviceNotifier.cpp
    services/WebcamDeviceNotifier.cpp
    services/DisplayDeviceNotifier.cpp
    services/CaptureTargetNotifier.cpp
    services/WebcamService.cpp
    # WebcamService calls exosnap::diagnostics::logEvent (NV12 open diagnostics);
    # every target that pulls in this lib needs the symbol, and this is the one
    # place all of them already link (RecordingCoordinator's own logEvent calls
    # ride along, since every RecordingCoordinator.cpp test target also links
    # this lib).
    diagnostics/StructuredLog.cpp
)
target_link_libraries(exosnap_device_service_test_support PUBLIC
    exosnap_app_log_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core
    ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib
)

exosnap_add_app_test_support(exosnap_pages_test_support
    pages/ConfigPage.cpp
    viewmodels/PresentationStateBuilder.cpp
    ui/widgets/ComboBoxWheelFilter.cpp
    ui/widgets/CameraPreview.cpp
    ui/widgets/ExoCheckBox.cpp
    ui/widgets/ExoToggle.cpp
    ui/widgets/InfoHintIcon.cpp
    # D6: CompareHint + compare data needed by ConfigPage.
    ui/widgets/CompareHint.cpp
    models/SettingsCompareData.cpp
    ui/widgets/SettingsCardExpander.cpp
    ui/widgets/VUMeterWidget.cpp
    ui/widgets/WebcamSetupPanel.cpp
    # v10: ConfigPage renders Lucide glyphs (card titles, row icons).
    ui/theme/LucideIcon.cpp
    # PS-PHASE-C: HotkeysSettingsPanel embedded in ConfigPage.
    ui/widgets/HotkeysSettingsPanel.cpp
    ui/widgets/KeycapChip.cpp
    services/GlobalHotkeyService.cpp
    ui/widgets/SectionRuleHeader.cpp
    ui/widgets/PlaceholderRow.cpp
    # THEME-SLICE-1: ExoCheckBox now calls ActiveTheme()/ParseThemeColor() from ExoSnapTheme.cpp.
    ui/theme/ExoSnapTheme.cpp
    # S3: ExoSlider used by ConfigPage (mic gain slider with gradient groove + ticks).
    ui/widgets/ExoSlider.cpp
)
target_link_libraries(exosnap_pages_test_support PUBLIC
    exosnap_device_service_test_support exosnap_output_model_test_support
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)

exosnap_add_app_test_support(exosnap_source_picker_test_support
    ui/dialogs/SourcePickerPanel.cpp
    ui/dialogs/SourcePickerDialog.cpp
    ui/dialogs/SourcePickerOverlay.cpp
    ui/widgets/RegionGeometry.cpp
    ui/widgets/CaptureTargetCard.cpp
    ui/widgets/RegionPresetCard.cpp
    # DESIGN-SYNC-R1: SourcePickerPanel's "Select region…" checkbox is now ExoCheckBox.
    ui/widgets/ExoCheckBox.cpp
    services/ThumbnailCapture.cpp
    services/CaptureSourceHub.cpp
    services/CaptureHubRegistry.cpp
    services/WgcSourceProducer.cpp
    # THEME-SLICE-2C: SourcePickerOverlay now calls ActiveTheme() from ExoSnapTheme.cpp.
    ui/theme/ExoSnapTheme.cpp
)
target_link_libraries(exosnap_source_picker_test_support PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets recorder_core d3d11.lib dxgi.lib runtimeobject.lib ole32.lib
)

# Only the pure evaluation half of the --hwnd-audit harness. The Win32 half needs
# a real MainWindow and therefore lives in the harness itself, not in a gtest —
# see HwndAuditHarness.h for why that split exists.
exosnap_add_gtest(
    NAME hwnd_audit_report_tests
    TEST_PREFIX hwnd_audit.
    SOURCES
        tests/test_hwnd_audit_report.cpp
        hwnd_audit/HwndAuditReport.cpp
    LIBRARIES Qt6::Core Qt6::Gui
)
target_include_directories(hwnd_audit_report_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME presentation_state_tests
    TEST_PREFIX state.
    SOURCES
        tests/test_presentation_state.cpp
        viewmodels/PresentationStateBuilder.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(presentation_state_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME completed_result_tests
    TEST_PREFIX result.
    SOURCES
        tests/test_completed_result.cpp
    LIBRARIES exosnap_record_viewmodel_test_support exosnap_capability recorder_core Qt6::Core Qt6::Gui Qt6::Widgets
)
target_include_directories(completed_result_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(completed_result_tests PRIVATE ole32.lib shell32.lib)
set_target_properties(completed_result_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME recording_history_store_tests
    TEST_PREFIX history_store.
    SOURCES
        tests/test_recording_history_store.cpp
    LIBRARIES exosnap_app_log_test_support exosnap_record_viewmodel_test_support exosnap_capability recorder_core Qt6::Core
)
target_include_directories(recording_history_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recording_history_store_tests PRIVATE ole32.lib shell32.lib)

exosnap_add_gtest(
    NAME recording_preset_tests
    TEST_PREFIX preset.
    SOURCES
        tests/test_recording_preset.cpp
        models/RecordingPreset.cpp
        models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(recording_preset_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recording_preset_tests PRIVATE ole32.lib shell32.lib)

# Stable display-identity ranked matcher + region normalization math (pure).
exosnap_add_gtest(
    NAME display_identity_resolver_tests
    TEST_PREFIX display_id.
    SOURCES
        tests/test_display_identity_resolver.cpp
        services/DisplayIdentityResolver.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(display_identity_resolver_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Exclusive-fullscreen pre-flight: pure window-shape + evidence-ladder resolvers (S1).
exosnap_add_gtest(
    NAME window_target_facts_tests
    TEST_PREFIX exclusive_fs.
    SOURCES
        tests/test_window_target_facts.cpp
        diagnostics/WindowTargetFacts.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(window_target_facts_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(window_target_facts_tests PRIVATE dwmapi.lib shell32.lib)

# Exclusive-fullscreen pre-flight: pure hub-evidence accumulator (S2a).
exosnap_add_gtest(
    NAME window_evidence_accumulator_tests
    TEST_PREFIX exclusive_fs.
    SOURCES
        tests/test_window_evidence_accumulator.cpp
        diagnostics/WindowEvidenceAccumulator.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(window_evidence_accumulator_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Exclusive-fullscreen mid-session stall predicate (S4, pure).
exosnap_add_gtest(
    NAME window_capture_stall_tests
    TEST_PREFIX exclusive_fs.
    SOURCES
        tests/test_window_capture_stall.cpp
        diagnostics/WindowCaptureStall.cpp
        diagnostics/WindowTargetFacts.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(window_capture_stall_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(window_capture_stall_tests PRIVATE dwmapi.lib shell32.lib)

# 0.5.0 video rate-control tests (ADR 0009): ComputeNvencRcParams mapping,
# preset round-trip, dirty-tracking, sanitize, and capability queries.
exosnap_add_gtest(
    NAME video_rate_control_tests
    TEST_PREFIX video_rc.
    SOURCES
        tests/test_video_rate_control.cpp
        models/RecordingPreset.cpp
        models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(video_rate_control_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(video_rate_control_tests PRIVATE ole32.lib shell32.lib)

# 0.5.0 audio encoding param tests (ADR 0019): preset round-trip, dirty-tracking, sanitize bounds.
exosnap_add_gtest(
    NAME audio_encoding_preset_tests
    TEST_PREFIX audio_enc.
    SOURCES
        tests/test_audio_encoding_preset.cpp
        settings/RecordingPresetStore.cpp
        models/RecordingPreset.cpp
        models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(audio_encoding_preset_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_encoding_preset_tests PRIVATE ole32.lib shell32.lib tomlplusplus::tomlplusplus)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    exosnap_add_gtest(
        NAME visual_scenario_tests
        TEST_PREFIX visual.
        SOURCES
            tests/test_visual_scenarios.cpp
            visual_tests/VisualScenario.cpp
        LIBRARIES Qt6::Core exosnap_capability recorder_core
    )
    target_include_directories(visual_scenario_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(visual_scenario_tests PRIVATE EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1)
endif()

exosnap_add_gtest(
    NAME record_viewmodel_audio_tests
    TEST_PREFIX viewmodel.
    SOURCES
        tests/test_record_view_model_audio.cpp
    LIBRARIES exosnap_record_viewmodel_test_support exosnap_capability recorder_core Qt6::Core
)
target_include_directories(record_viewmodel_audio_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME region_model_tests
    TEST_PREFIX region.
    SOURCES
        tests/test_region_model.cpp
    LIBRARIES recorder_core
)
target_include_directories(region_model_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME recording_countdown_tests
    TEST_PREFIX countdown.
    SOURCES
        tests/test_recording_countdown.cpp
        services/RecordingCountdownController.cpp
)
target_include_directories(recording_countdown_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME page_hydration_controller_tests
    TEST_PREFIX hydration.
    SOURCES
        tests/test_page_hydration_controller.cpp
        services/PageHydrationController.cpp
        diagnostics/AppLog.cpp
    LIBRARIES Qt6::Core
)
set_target_properties(page_hydration_controller_tests PROPERTIES AUTOMOC ON)
target_include_directories(page_hydration_controller_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME region_geometry_tests
    TEST_PREFIX region.
    SOURCES
        tests/test_region_geometry.cpp
        ui/widgets/RegionGeometry.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(region_geometry_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME window_geometry_policy_tests
    TEST_PREFIX window.
    SOURCES
        tests/test_window_geometry_policy.cpp
        ui/WindowGeometryPolicy.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(window_geometry_policy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME error_message_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_error_message.cpp
        diagnostics/error_message.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(error_message_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Engine -> AppLog bridge: drives the real InitializeEngineLogging() + a genuine
# recorder_core::logging::log() call, so a green test means engine records really
# reach the application log.
exosnap_add_gtest(
    NAME engine_log_bridge_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_engine_log_bridge.cpp
        diagnostics/EngineLogBridge.cpp
        diagnostics/AppLog.cpp
    LIBRARIES recorder_core Qt6::Core
)
target_include_directories(engine_log_bridge_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(engine_log_bridge_tests PROPERTIES AUTOMOC ON)

# Structured-event entry point: logEvent forwards only to the engine JSONL, so the
# bridge is the single source of the text line (no double-write), and a Debug event
# is dropped at the engine's Info minimumLevel.
exosnap_add_gtest(
    NAME structured_log_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_structured_log.cpp
        diagnostics/StructuredLog.cpp
        diagnostics/EngineLogBridge.cpp
        diagnostics/AppLog.cpp
    LIBRARIES recorder_core Qt6::Core
)
target_include_directories(structured_log_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(structured_log_tests PROPERTIES AUTOMOC ON)

# Session report: the pure JSON builder (deterministic, honest unavailable metrics,
# segments, error phase, peak drift) and the writer's prune-to-keep-N.
exosnap_add_gtest(
    NAME session_report_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_session_report.cpp
        diagnostics/SessionReport.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core Qt6::Gui
)
target_include_directories(session_report_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(session_report_tests PRIVATE ole32.lib shell32.lib)

# ZIP writer over the vendored miniz: round-trips through libs/update's ExtractZip
# (the reader side) and enforces the reused entry-name safety guard.
exosnap_add_gtest(
    NAME zip_writer_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_zip_writer.cpp
        diagnostics/ZipWriter.cpp
    LIBRARIES exosnap_miniz exosnap::update Qt6::Core
)
target_include_directories(zip_writer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Support bundle: the entry set, and the privacy regression guard (no path,
# username, machine, output path OR capture-target window title in any entry).
exosnap_add_gtest(
    NAME support_bundle_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_support_bundle.cpp
        diagnostics/SupportBundle.cpp
        diagnostics/ZipWriter.cpp
    LIBRARIES exosnap_miniz exosnap::update exosnap::crash_capture Qt6::Core
)
target_include_directories(support_bundle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Startup trace collector (header-only): ordering, de-duplication, formatter.
exosnap_add_gtest(
    NAME startup_trace_tests
    TEST_PREFIX diagnostics.
    SOURCES tests/test_startup_trace.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(startup_trace_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# App log file rotation: size-based rotation, handle-reuse across writes, and
# Logs-page (in-memory history) compatibility across rotation.
exosnap_add_gtest(
    NAME app_log_rotation_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_app_log_rotation.cpp
        diagnostics/AppLog.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(app_log_rotation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(app_log_rotation_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME output_settings_tests
    TEST_PREFIX output.
    SOURCES
        tests/test_output_settings.cpp
        services/RecordingCoordinator.cpp
        services/AtomicFileOps.cpp
        services/DisplayNumbering.cpp
        settings/RecoveryManifestStore.cpp
    LIBRARIES
        exosnap_output_model_test_support
        exosnap_device_service_test_support
        exosnap_diagnostics_test_support
        Qt6::Core Qt6::Gui exosnap_capability recorder_core
)
target_include_directories(output_settings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# ADR-0014: mfreadwrite.lib removed (MF/SinkWriter path deleted).
target_link_libraries(output_settings_tests PRIVATE mfplat.lib mf.lib mfuuid.lib ole32.lib)
set_target_properties(output_settings_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME auto_record_harness_tests
    TEST_PREFIX auto_record.
    SOURCES
        tests/test_auto_record_harness.cpp
        auto_record/AutoRecordOptions.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(auto_record_harness_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# MP4-SPLIT-REMUX-R1: per-segment manifest lifecycle, disk reserve, split settings.
exosnap_add_gtest(
    NAME mp4_split_remux_tests
    TEST_PREFIX split_remux.
    SOURCES
        tests/test_mp4_split_remux.cpp
        services/RecordingCoordinator.cpp
        services/AtomicFileOps.cpp
        services/DisplayNumbering.cpp
        settings/RecoveryManifestStore.cpp
    LIBRARIES
        exosnap_output_model_test_support
        exosnap_device_service_test_support
        exosnap_diagnostics_test_support
        Qt6::Core Qt6::Gui exosnap_capability recorder_core
)
target_include_directories(mp4_split_remux_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mp4_split_remux_tests PRIVATE mfplat.lib mf.lib mfuuid.lib ole32.lib)
set_target_properties(mp4_split_remux_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME app_settings_store_tests
    TEST_PREFIX settings.
    SOURCES
        tests/test_app_settings_store.cpp
        settings/AppSettingsStore.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(app_settings_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Theme table invariant tests. Headless — no QApplication required; only
# inspects ExoTheme + ExoSnapPalette.
exosnap_add_gtest(
    NAME accent_theme_tests
    TEST_PREFIX accent.
    SOURCES
        tests/test_accent_theme.cpp
    LIBRARIES Qt6::Core Qt6::Gui
)
target_include_directories(accent_theme_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME diagnostics_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_diagnostics.cpp
    LIBRARIES exosnap_diagnostics_test_support exosnap_capability recorder_core
)
target_include_directories(diagnostics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# LOW-DISK-GUARD-R1 tests
exosnap_add_gtest(
    NAME low_disk_guard_tests
    TEST_PREFIX disk.
    SOURCES
        tests/test_low_disk_guard.cpp
    LIBRARIES exosnap_diagnostics_test_support exosnap_capability recorder_core Qt6::Core
)
target_include_directories(low_disk_guard_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# FILESYSTEM-CHECKS-R1 tests
exosnap_add_gtest(
    NAME filesystem_checks_tests
    TEST_PREFIX fs.
    SOURCES
        tests/test_filesystem_checks.cpp
    LIBRARIES exosnap_diagnostics_test_support exosnap_capability recorder_core
)
target_include_directories(filesystem_checks_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# webcam_settings_tests removed: webcam is now persisted via RecordingPresetStore.
# Coverage of WebcamSettings serialization is maintained by recording_preset_store_tests.

exosnap_add_gtest(
    NAME global_recording_bar_tests
    TEST_PREFIX chrome.
    SOURCES
        tests/test_global_recording_bar.cpp
        ui/chrome/GlobalRecordingBar.cpp
        ui/widgets/StatusPill.cpp
        # THEME-SLICE-2C: StatusPill now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(global_recording_bar_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(global_recording_bar_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME operational_title_bar_tests
    TEST_PREFIX chrome.
    SOURCES
        tests/test_operational_title_bar.cpp
        ui/chrome/OperationalTitleBar.cpp
        ui/brand/BrandMarkWidget.cpp
        ui/widgets/NotificationBell.cpp
        ui/widgets/StatusPill.cpp
        ui/theme/LucideIcon.cpp
        # THEME-SLICE-1: BrandMarkWidget now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core
)
target_include_directories(operational_title_bar_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(operational_title_bar_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME config_page_tests
    TEST_PREFIX config.
    SOURCES
        tests/test_config_page.cpp
    LIBRARIES exosnap_pages_test_support Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(config_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(config_page_tests PROPERTIES AUTOMOC ON)
target_link_libraries(config_page_tests PRIVATE mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib ole32.lib)

# SETTINGS-TIERS-R2 Phase 2: InfoHintIcon widget unit tests.
exosnap_add_gtest(
    NAME info_hint_icon_tests
    TEST_PREFIX hint.
    SOURCES
        tests/test_info_hint_icon.cpp
        ui/widgets/InfoHintIcon.cpp
        ui/theme/LucideIcon.cpp
        # THEME-SLICE-2C: InfoHintIcon now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(info_hint_icon_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(info_hint_icon_tests PROPERTIES AUTOMOC ON)

# PS-FOUNDATIONS-R1: Suite foundation widget tests.
exosnap_add_gtest(
    NAME exo_spin_box_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_exo_spin_box.cpp
        ui/widgets/ExoSpinBox.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets
)
target_include_directories(exo_spin_box_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(exo_spin_box_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME exo_slider_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_exo_slider.cpp
        ui/widgets/ExoSlider.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets
)
target_include_directories(exo_slider_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(exo_slider_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME notification_bell_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_notification_bell.cpp
        ui/widgets/NotificationBell.cpp
        ui/theme/LucideIcon.cpp
        # THEME-SLICE-2C: NotificationBell now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(notification_bell_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(notification_bell_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME advisory_item_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_advisory_item.cpp
        ui/widgets/AdvisoryItem.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(advisory_item_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(advisory_item_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME notification_hub_panel_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_notification_hub_panel.cpp
        ui/chrome/NotificationHubPanel.cpp
        ui/widgets/AdvisoryItem.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(notification_hub_panel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(notification_hub_panel_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME corner_capture_button_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_corner_capture_button.cpp
        ui/widgets/CornerCaptureButton.cpp
        ui/theme/LucideIcon.cpp
        # THEME-SLICE-2C: CornerCaptureButton now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(corner_capture_button_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(corner_capture_button_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME codec_labels_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_codec_labels.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(codec_labels_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME placeholder_row_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_placeholder_row.cpp
        ui/widgets/PlaceholderRow.cpp
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets
)
target_include_directories(placeholder_row_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(placeholder_row_tests PROPERTIES AUTOMOC ON)

# Settings-Redesign D6: CompareHint widget + compare data unit tests.
exosnap_add_gtest(
    NAME compare_hint_tests
    TEST_PREFIX compare.
    SOURCES
        tests/test_compare_hint.cpp
        ui/widgets/CompareHint.cpp
        models/SettingsCompareData.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(compare_hint_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(compare_hint_tests PROPERTIES AUTOMOC ON)

# SETTINGS-TIERS-R2 Phase 2: ConfigPage InfoHint integration tests.
exosnap_add_gtest(
    NAME info_hints_config_tests
    TEST_PREFIX hint.
    SOURCES
        tests/test_info_hints_config.cpp
    LIBRARIES exosnap_pages_test_support Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(info_hints_config_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(info_hints_config_tests PROPERTIES AUTOMOC ON)
target_link_libraries(info_hints_config_tests PRIVATE mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib ole32.lib)

# SETTINGS-TIERS-R1: Expert mode toggle, per-card expander, and persistence tests.
exosnap_add_gtest(
    NAME settings_tiers_tests
    TEST_PREFIX tiers.
    SOURCES
        tests/test_settings_tiers.cpp
        settings/AppSettingsStore.cpp
    LIBRARIES exosnap_pages_test_support Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(settings_tiers_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(settings_tiers_tests PROPERTIES AUTOMOC ON)
target_link_libraries(settings_tiers_tests PRIVATE mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib ole32.lib)

# ELEVATION-FOUNDATION-R1 (ADR 0033): elevation provider stub + relaunch handoff
# arg builder/parser + RelaunchResult contract. Pure logic; no Win32 invocation.
exosnap_add_gtest(
    NAME elevation_tests
    TEST_PREFIX elevation.
    SOURCES
        tests/test_elevation.cpp
        services/ElevatedRelaunch.cpp
    LIBRARIES Qt6::Core Shell32.lib
)
target_include_directories(elevation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(elevation_tests PRIVATE NOMINMAX)

# ADR 0033: present-diagnostics provider. PresentMonProvider owns a
# PresentMonEtwSession and gates on elevation && opt-in before starting it.
# EXOSNAP_HAS_PRESENTMON is intentionally NOT defined here so the no-op session
# is used, keeping this target Qt-only (no ETW dep) while the gate logic is
# fully covered.
exosnap_add_gtest(
    NAME present_provider_tests
    TEST_PREFIX present.
    SOURCES
        tests/test_present_provider.cpp
        diagnostics/PresentMonProvider.cpp
        diagnostics/PresentMonEtwSession.cpp
        diagnostics/PresentModeMapping.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(present_provider_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Pure PresentMon present-mode mapping. Maps raw present events (POD) to our
# diagnostics::PresentSample. No Win32/ETW headers; unit-testable in isolation.
exosnap_add_gtest(
    NAME present_mapping_tests
    TEST_PREFIX present_mapping.
    SOURCES
        tests/test_present_mode_mapping.cpp
        diagnostics/PresentModeMapping.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(present_mapping_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME source_picker_dialog_tests
    TEST_PREFIX source.
    SOURCES
        tests/test_source_picker_dialog.cpp
    LIBRARIES exosnap_source_picker_test_support Qt6::Core Qt6::Gui Qt6::Widgets d3d11.lib dxgi.lib runtimeobject.lib ole32.lib
)
target_include_directories(source_picker_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(source_picker_dialog_tests PRIVATE NOMINMAX)
set_target_properties(source_picker_dialog_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME source_picker_overlay_tests
    TEST_PREFIX source.
    SOURCES
        tests/test_source_picker_overlay.cpp
    LIBRARIES exosnap_source_picker_test_support Qt6::Core Qt6::Gui Qt6::Widgets d3d11.lib dxgi.lib runtimeobject.lib ole32.lib
)
target_include_directories(source_picker_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(source_picker_overlay_tests PRIVATE NOMINMAX)
set_target_properties(source_picker_overlay_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME source_picker_refresh_tests
    TEST_PREFIX source.
    SOURCES
        tests/test_source_picker_refresh.cpp
    LIBRARIES exosnap_source_picker_test_support Qt6::Core Qt6::Gui Qt6::Widgets d3d11.lib dxgi.lib runtimeobject.lib ole32.lib
)
target_include_directories(source_picker_refresh_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(source_picker_refresh_tests PRIVATE NOMINMAX)
set_target_properties(source_picker_refresh_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME webcam_setup_panel_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_setup_panel.cpp
    LIBRARIES exosnap_pages_test_support Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(webcam_setup_panel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(webcam_setup_panel_tests PROPERTIES AUTOMOC ON)
target_link_libraries(webcam_setup_panel_tests PRIVATE mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib ole32.lib)

exosnap_add_gtest(
    NAME hotkeys_settings_panel_tests
    TEST_PREFIX hotkey.
    SOURCES
        tests/test_hotkeys_settings_panel.cpp
        ui/widgets/HotkeysSettingsPanel.cpp
        services/GlobalHotkeyService.cpp
        ui/widgets/KeycapChip.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(hotkeys_settings_panel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(hotkeys_settings_panel_tests PRIVATE EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1)
set_target_properties(hotkeys_settings_panel_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME hotkey_service_tests
    TEST_PREFIX hotkey_service.
    SOURCES
        tests/test_global_hotkey_service.cpp
        services/GlobalHotkeyService.cpp
    LIBRARIES Qt6::Core Qt6::Gui exosnap_capability recorder_core
)
target_include_directories(hotkey_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(hotkey_service_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME logs_page_tests
    TEST_PREFIX log.
    SOURCES
        tests/test_logs_page.cpp
        pages/LogsPage.cpp
        diagnostics/AppLog.cpp
        ui/widgets/SectionRuleHeader.cpp
        # DESIGN-SYNC-R1: LogsPage's "Auto-scroll" checkbox is now ExoCheckBox, which
        # calls ActiveTheme()/ParseThemeColor() from ExoSnapTheme.cpp.
        ui/widgets/ExoCheckBox.cpp
        ui/theme/ExoSnapTheme.cpp
        # v10: LogsPage's search field uses lucideIcon() (Lucide glyph renderer).
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core
)
target_include_directories(logs_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(logs_page_tests PRIVATE EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1)
set_target_properties(logs_page_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME about_page_tests
    TEST_PREFIX about.
    SOURCES
        tests/test_about_page.cpp
        pages/AboutPage.cpp
        models/AboutInfo.cpp
        ui/brand/BrandMarkWidget.cpp
        ui/theme/LucideIcon.cpp
        # THEME-SLICE-1: BrandMarkWidget now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
        # AboutPage resolves install mode (MSI/Portable/Scoop) via these pure helpers.
        services/UpdateLaunchPlan.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap::update
)
target_include_directories(about_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(about_page_tests PRIVATE EXOSNAP_BUILD_CONFIG="$<CONFIG>")
add_dependencies(about_page_tests exosnap_build_info)
set_target_properties(about_page_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME edit_export_page_tests
    TEST_PREFIX edit_export.
    SOURCES
        tests/test_edit_export_page.cpp
        pages/EditExportPage.cpp
        ui/widgets/EditTimeline.cpp
        services/TimelineThumbnailSource.h
        services/TimelineThumbnailSource.cpp
        ui/widgets/EditPlayerSurface.h
        ui/widgets/EditPlayerSurface.cpp
        # Native child-HWND GPU render host EditPlayerSurface now owns (2026-08-03
        # editor-playback-gpu-render design) -- see edit_player_surface_tests below
        # for its own dedicated lifecycle/resize coverage.
        services/EditPlayerRenderer.h
        services/EditPlayerRenderer.cpp
        diagnostics/AppLog.h
        diagnostics/AppLog.cpp
        ui/widgets/EditDetailsRail.h
        ui/widgets/EditDetailsRail.cpp
        ui/widgets/ExportPanel.h
        ui/widgets/ExportPanel.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg recorder_core
        d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib
)
target_include_directories(edit_export_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(edit_export_page_tests PRIVATE NOMINMAX)
set_target_properties(edit_export_page_tests PROPERTIES AUTOMOC ON)

# EDIT-OVERLAY-R1: EditExportOverlay hosts EditExportPage as an overlay over Record
# (ADR 0022 update) instead of a stack page — same technical recipe as
# SourcePickerOverlay (see source_picker_overlay_tests below).
exosnap_add_gtest(
    NAME edit_export_overlay_tests
    TEST_PREFIX edit_export_overlay.
    SOURCES
        tests/test_edit_export_overlay.cpp
        ui/dialogs/EditExportOverlay.cpp
        pages/EditExportPage.cpp
        ui/widgets/EditTimeline.cpp
        services/TimelineThumbnailSource.h
        services/TimelineThumbnailSource.cpp
        ui/widgets/EditPlayerSurface.h
        ui/widgets/EditPlayerSurface.cpp
        services/EditPlayerRenderer.h
        services/EditPlayerRenderer.cpp
        diagnostics/AppLog.h
        diagnostics/AppLog.cpp
        ui/widgets/EditDetailsRail.h
        ui/widgets/EditDetailsRail.cpp
        ui/widgets/ExportPanel.h
        ui/widgets/ExportPanel.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg recorder_core
        d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib
)
target_include_directories(edit_export_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(edit_export_overlay_tests PRIVATE NOMINMAX)
set_target_properties(edit_export_overlay_tests PROPERTIES AUTOMOC ON)

# Native child-HWND GPU render host for the editor player (2026-08-03
# editor-playback-gpu-render design, Task 4): EditPlayerSurface's own
# lifecycle/resize coverage, dedicated (rather than folded into
# edit_export_page_tests above) so it can run against a real hardware D3D11
# device -- same live-hardware precedent as dxgi_preview_pushed_source_tests
# (EditPlayerRenderer::InitD3D11() hardcodes D3D_DRIVER_TYPE_HARDWARE);
# GTEST_SKIP covers headless/GPU-less runners.
exosnap_add_gtest(
    NAME edit_player_surface_tests
    TEST_PREFIX edit_player.
    SOURCES
        tests/test_edit_player_surface.cpp
        ui/widgets/EditPlayerSurface.h
        ui/widgets/EditPlayerSurface.cpp
        services/EditPlayerRenderer.h
        services/EditPlayerRenderer.cpp
        diagnostics/AppLog.h
        diagnostics/AppLog.cpp
    LIBRARIES
        Qt6::Core Qt6::Gui Qt6::Widgets recorder_core
        d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib
    LABELS live
)
target_include_directories(edit_player_surface_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(edit_player_surface_tests PRIVATE NOMINMAX)
set_target_properties(edit_player_surface_tests PROPERTIES AUTOMOC ON)

# Export panel embedded in the edit surface's right rail; presentation only, the
# export itself stays in EditExportPage (2026-08-03 edit-export-panel design).
exosnap_add_gtest(
    NAME export_panel_tests
    TEST_PREFIX export_panel.
    SOURCES
        tests/test_export_panel.cpp
        ui/widgets/ExportPanel.h
        ui/widgets/ExportPanel.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(export_panel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(export_panel_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME edit_details_rail_tests
    TEST_PREFIX edit_details_rail.
    SOURCES
        tests/test_edit_details_rail.cpp
        ui/widgets/EditDetailsRail.h
        ui/widgets/EditDetailsRail.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(edit_details_rail_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(edit_details_rail_tests PROPERTIES AUTOMOC ON)

# Strip layout + tile loop behind the Edit timeline's thumbnails. The decoder is
# injected, so these cases run without a clip, a worker thread, or FFmpeg.
exosnap_add_gtest(
    NAME timeline_thumbnail_source_tests
    TEST_PREFIX timeline_thumbnails.
    SOURCES
        tests/test_timeline_thumbnail_source.cpp
        services/TimelineThumbnailSource.h
        services/TimelineThumbnailSource.cpp
    LIBRARIES Qt6::Core Qt6::Gui recorder_core
)
target_include_directories(timeline_thumbnail_source_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(timeline_thumbnail_source_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME test_edit_context
    TEST_PREFIX edit_context.
    SOURCES tests/test_edit_context.cpp
    LIBRARIES Qt6::Core Qt6::Gui
)
target_include_directories(test_edit_context PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Pure logic behind the Edit timeline (timestamp formatting, trim clamping,
# marker retiming) and the marker sidecar that rides along exports.
exosnap_add_gtest(
    NAME edit_timeline_model_tests
    TEST_PREFIX edit_timeline_model.
    SOURCES tests/test_edit_timeline_model.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(edit_timeline_model_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME crash_report_overlay_tests
    TEST_PREFIX crash.
    SOURCES
        tests/test_crash_report_overlay.cpp
        ui/dialogs/CrashReportPanel.cpp
        ui/dialogs/CrashReportOverlay.cpp
        ui/brand/BrandMarkWidget.cpp
        ui/theme/LucideIcon.cpp
        # THEME-SLICE-1: BrandMarkWidget now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
        # DESIGN-SYNC-R1: CrashReportPanel's remember-choice draft is an ExoCheckBox.
        ui/widgets/ExoCheckBox.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(crash_report_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(crash_report_overlay_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME recording_error_overlay_tests
    TEST_PREFIX record.
    SOURCES
        tests/test_recording_error_overlay.cpp
        ui/dialogs/RecordingErrorPanel.cpp
        ui/dialogs/RecordingErrorOverlay.cpp
        ui/theme/LucideIcon.cpp
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(recording_error_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(recording_error_overlay_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME lucide_icon_tests
    TEST_PREFIX lucide.
    SOURCES
        tests/test_lucide_icon.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(lucide_icon_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(lucide_icon_tests PROPERTIES AUTOMOC ON)

# Pure helpers behind UpdateService::LaunchUpdater: staging file list, updater
# argv (round-tripped through the updater's own parser), Scoop-path detection.
exosnap_add_gtest(
    NAME update_launch_plan_tests
    TEST_PREFIX update_launch.
    SOURCES
        tests/test_update_launch_plan.cpp
        services/UpdateLaunchPlan.cpp
        services/VerifyReinstallMode.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../apps/updater/UpdaterArgs.cpp
    LIBRARIES Qt6::Core exosnap::update
)
target_include_directories(update_launch_plan_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

exosnap_add_gtest(
    NAME version_tests
    TEST_PREFIX version.
    SOURCES
        tests/test_version.cpp
)
target_include_directories(version_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(version_tests PRIVATE
    EXOSNAP_EXPECTED_VERSION="${EXOSNAP_FULL_VERSION}"
    EXOSNAP_EXPECTED_BASE_VERSION="${PROJECT_VERSION}")
add_dependencies(version_tests exosnap_build_info)

exosnap_add_gtest(
    NAME preview_helpers_tests
    TEST_PREFIX preview.
    SOURCES
        tests/test_preview_helpers.cpp
    LIBRARIES recorder_core
)
target_include_directories(preview_helpers_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Pure switch-over state machine for the WYSIWYG preview during recording
# (pushed engine source vs the preview's own WGC capture). Header-only logic.
exosnap_add_gtest(
    NAME pushed_source_state_tests
    TEST_PREFIX preview.
    SOURCES
        tests/test_pushed_source_state.cpp
)
target_include_directories(pushed_source_state_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME region_preview_crop_tests
    TEST_PREFIX preview.
    SOURCES
        tests/test_region_preview_crop.cpp
    LIBRARIES recorder_core
)
target_include_directories(region_preview_crop_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME webcam_placement_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_placement.cpp
    LIBRARIES recorder_core
)
target_include_directories(webcam_placement_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME preview_surface_webcam_tests
    TEST_PREFIX preview.
    SOURCES
        tests/test_preview_surface_webcam.cpp
        ui/widgets/PreviewSurface.cpp
        ui/widgets/StatusPill.cpp
        services/DxgiPreviewRenderer.cpp
        diagnostics/AppLog.cpp
        # THEME-SLICE-2C: StatusPill now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
        # Webcam magnifier: PreviewSurface::paintMagnifierIcon() renders the
        # search/x glyph via lucidePixmap().
        ui/theme/LucideIcon.cpp
    LIBRARIES
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core
        d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib
)
target_include_directories(preview_surface_webcam_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(preview_surface_webcam_tests PRIVATE NOMINMAX)
set_target_properties(preview_surface_webcam_tests PROPERTIES AUTOMOC ON)

# Regression coverage for the webcam-PiP handle/box desync bug: DxgiPreviewRenderer::
# GetSourceSize() must track whatever the render thread is CURRENTLY drawing as the
# pushed-source background, not a stale value from before the current source was
# adopted. Exercises the real BeginPushedSource/keyed-mutex handoff against a live
# hardware D3D11 device (InitD3D11() hardcodes D3D_DRIVER_TYPE_HARDWARE), so it is
# GPU-live like capability_adapter_enum_tests; GTEST_SKIP covers headless runners.
exosnap_add_gtest(
    NAME dxgi_preview_pushed_source_tests
    TEST_PREFIX preview.
    SOURCES
        tests/test_dxgi_preview_pushed_source.cpp
        services/DxgiPreviewRenderer.cpp
        diagnostics/AppLog.cpp
    LIBRARIES
        Qt6::Core recorder_core
        d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib
    LABELS live
)
target_include_directories(dxgi_preview_pushed_source_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dxgi_preview_pushed_source_tests PRIVATE NOMINMAX)
set_target_properties(dxgi_preview_pushed_source_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME transport_dock_tests
    TEST_PREFIX record.
    SOURCES
        tests/test_transport_dock.cpp
        ui/widgets/TransportDock.cpp
        ui/widgets/AudioSourceToggle.cpp
        # THEME-SLICE-2C: AudioSourceToggle now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core
)
target_include_directories(transport_dock_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(transport_dock_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME pipeline_flow_tests
    TEST_PREFIX diagnostic.
    SOURCES
        tests/test_pipeline_flow.cpp
        ui/widgets/PipelineFlow.cpp
        ui/widgets/PipelineStepCard.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets
)
target_include_directories(pipeline_flow_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(pipeline_flow_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME diagnostics_page_tests
    TEST_PREFIX diagnostic.
    SOURCES
        tests/test_diagnostics_page.cpp
        pages/DiagnosticsPage.cpp
        ui/widgets/SectionRuleHeader.cpp
        ui/widgets/PipelineFlow.cpp
        ui/widgets/PipelineStepCard.cpp
        ui/widgets/LivePipelinePanel.cpp
        ui/widgets/TipChip.cpp
        ui/widgets/ElevationLock.cpp
        # Diagnostics redesign: Expert toggle mirrors Settings (ExoToggle → ActiveTheme()).
        ui/widgets/ExoToggle.cpp
        ui/theme/ExoSnapTheme.cpp
        # v10: DiagnosticsPage's readiness chip uses lucidePixmap() (Lucide glyph renderer).
        ui/theme/LucideIcon.cpp
        # ADR 0033: present-mode provider bridge. No EXOSNAP_HAS_PRESENTMON define →
        # PresentMonEtwSession compiles the graceful no-op stub branch (no ETW dep).
        diagnostics/PresentMonProvider.cpp
        diagnostics/PresentMonEtwSession.cpp
        diagnostics/PresentModeMapping.cpp
        # ADR 0033: DPC/ISR latency provider. Same no-op stub branch (no ETW dep) since
        # EXOSNAP_HAS_PRESENTMON is not defined here; DiagnosticsPage samples Read().
        diagnostics/DpcLatencyProvider.cpp
    LIBRARIES exosnap_diagnostics_test_support Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core
)
target_include_directories(diagnostics_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(diagnostics_page_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME device_page_tests
    TEST_PREFIX device.
    SOURCES
        tests/test_device_page.cpp
        pages/DevicePage.cpp
        ui/widgets/DeviceAdapterCard.cpp
        ui/widgets/SectionRuleHeader.cpp
        # DevicePage's adapter avatar + codec-chip icons use lucidePixmap() (Lucide glyph renderer).
        ui/theme/LucideIcon.cpp
    # exosnap_app_log_test_support: DevicePage logs the device-rescan-start/end perf
    # milestones via AppLog (perf/startup-milestones slice).
    LIBRARIES exosnap_app_log_test_support Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core
)
target_include_directories(device_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(device_page_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME recording_preset_registry_tests
    TEST_PREFIX preset.
    SOURCES tests/test_recording_preset_registry.cpp
            models/RecordingPresetRegistry.cpp
            models/RecordingPreset.cpp
            models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core)
target_include_directories(recording_preset_registry_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recording_preset_registry_tests PRIVATE ole32.lib shell32.lib)

exosnap_add_gtest(
    NAME recording_preset_store_tests
    TEST_PREFIX preset.
    SOURCES tests/test_recording_preset_store.cpp
            settings/RecordingPresetStore.cpp
            models/RecordingPreset.cpp
            models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core)
target_include_directories(recording_preset_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recording_preset_store_tests PRIVATE ole32.lib shell32.lib tomlplusplus::tomlplusplus)

# PRESET-EXPORT-IMPORT-R1 tests
exosnap_add_gtest(
    NAME preset_export_import_tests
    TEST_PREFIX preset.
    SOURCES tests/test_preset_export_import.cpp
            settings/RecordingPresetStore.cpp
            models/RecordingPreset.cpp
            models/RecordingPresetRegistry.cpp
            models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core)
target_include_directories(preset_export_import_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(preset_export_import_tests PRIVATE ole32.lib shell32.lib tomlplusplus::tomlplusplus)

# Audio v2 (0.6.0): per-row gain + mute preset round-trip, dirty-tracking, sanitize.
exosnap_add_gtest(
    NAME audio_gain_preset_tests
    TEST_PREFIX audio_gain.
    SOURCES tests/test_audio_gain_preset.cpp
            settings/RecordingPresetStore.cpp
            models/RecordingPreset.cpp
            models/OutputSettingsModel.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core)
target_include_directories(audio_gain_preset_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_gain_preset_tests PRIVATE ole32.lib shell32.lib tomlplusplus::tomlplusplus)

# Audio v2 (0.6.0): per-row gain/mute handler wiring tests (view-model level, headless).
exosnap_add_gtest(
    NAME audio_row_gain_handler_tests
    TEST_PREFIX audio_gain_handler.
    SOURCES tests/test_audio_row_gain_handlers.cpp
    LIBRARIES exosnap_record_viewmodel_test_support exosnap_capability recorder_core Qt6::Core)
target_include_directories(audio_row_gain_handler_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# RECOVERY-MANIFEST-R1 tests
exosnap_add_gtest(
    NAME recovery_manifest_store_tests
    TEST_PREFIX recovery.
    SOURCES tests/test_recovery_manifest_store.cpp
            settings/RecoveryManifestStore.cpp
            diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(recovery_manifest_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(recovery_manifest_store_tests PROPERTIES AUTOMOC ON)

# Capability warm-start disk cache: roundtrip, key-mismatch, and corrupt-file tests.
exosnap_add_gtest(
    NAME capability_cache_store_tests
    TEST_PREFIX capability_cache.
    SOURCES tests/test_capability_cache_store.cpp
            settings/CapabilityCacheStore.cpp
            diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(capability_cache_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(capability_cache_store_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME recovery_service_tests
    TEST_PREFIX recovery.
    SOURCES tests/test_recovery_service.cpp
            settings/RecoveryManifestStore.cpp
            services/RecoveryService.cpp
            services/AtomicFileOps.cpp
            diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core FFmpeg::mux
)
target_include_directories(recovery_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(recovery_service_tests PROPERTIES AUTOMOC ON)

# Recovery drill matrix: drive RecoveryService against REAL synthetic-pipeline
# artefacts (real MKV, truncated to model a kill). Includes the Remux×Kill drill
# that asserts recovery replaces a corrupt half-MP4 at the target path in place.
exosnap_add_gtest(
    NAME recovery_drill_tests
    TEST_PREFIX recovery.
    SOURCES tests/test_recovery_drills.cpp
            settings/RecoveryManifestStore.cpp
            services/RecoveryService.cpp
            services/AtomicFileOps.cpp
            diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core recorder_core_testutil Qt6::Core FFmpeg::mux
)
target_include_directories(recovery_drill_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(recovery_drill_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME recovery_overlay_tests
    TEST_PREFIX recovery.
    SOURCES
        tests/test_recovery_overlay.cpp
        ui/dialogs/RecoveryOverlay.cpp
        settings/RecoveryManifestStore.cpp
        services/RecoveryService.cpp
        services/AtomicFileOps.cpp
        diagnostics/AppLog.cpp
        ui/brand/BrandMarkWidget.cpp
        # THEME-SLICE-2C: RecoveryOverlay now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core FFmpeg::mux
)
target_include_directories(recovery_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(recovery_overlay_tests PRIVATE NOMINMAX)
set_target_properties(recovery_overlay_tests PROPERTIES AUTOMOC ON)

# WHATS-NEW: pending-payload persistence round-trip + show decision (headless).
exosnap_add_gtest(
    NAME whats_new_payload_tests
    TEST_PREFIX whats_new.
    SOURCES
        tests/test_whats_new_payload.cpp
        services/WhatsNewPayload.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(whats_new_payload_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# WHATS-NEW: overlay smoke (sections render, checkbox only post-update, Esc/close).
exosnap_add_gtest(
    NAME whats_new_overlay_tests
    TEST_PREFIX whats_new.
    SOURCES
        tests/test_whats_new_overlay.cpp
        ui/dialogs/WhatsNewOverlay.cpp
        services/WhatsNewPayload.cpp
        ui/widgets/ExoCheckBox.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(whats_new_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(whats_new_overlay_tests PRIVATE NOMINMAX)
set_target_properties(whats_new_overlay_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME audio_device_notifier_tests
    TEST_PREFIX device.
    SOURCES
        tests/test_audio_device_notifier.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core exosnap_capability recorder_core)
target_include_directories(audio_device_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_device_notifier_tests PRIVATE ole32.lib)
set_target_properties(audio_device_notifier_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME webcam_device_notifier_tests
    TEST_PREFIX device.
    SOURCES
        tests/test_webcam_device_notifier.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(webcam_device_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webcam_device_notifier_tests PRIVATE ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)
set_target_properties(webcam_device_notifier_tests PROPERTIES AUTOMOC ON)

# Pure webcam ReadSample loss-detection policy (ClassifyWebcamReadResult).
# No live device / QApplication needed — links the service support target only for
# the classifier symbol.
exosnap_add_gtest(
    NAME webcam_read_policy_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_read_policy.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(webcam_read_policy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webcam_read_policy_tests PRIVATE ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)
set_target_properties(webcam_read_policy_tests PROPERTIES AUTOMOC ON)

# Pure copy mapping for the dock's webcam open-failure tooltip
# (FriendlyWebcamOpenFailure) — string logic only, no MF calls or live device.
exosnap_add_gtest(
    NAME webcam_open_failure_copy_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_open_failure_copy.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(webcam_open_failure_copy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webcam_open_failure_copy_tests PRIVATE ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)
set_target_properties(webcam_open_failure_copy_tests PROPERTIES AUTOMOC ON)

# The hub's loop against a fake producer: refcounted open/close, the held frame
# across loss and unbounded reopen, and the lease handover. No GPU, no WGC, no
# threads -- Pump() is stepped by hand.
exosnap_add_gtest(
    NAME capture_source_hub_tests
    TEST_PREFIX capture.
    SOURCES
        tests/test_capture_source_hub.cpp
        services/CaptureSourceHub.cpp
    LIBRARIES recorder_core)
target_include_directories(capture_source_hub_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Delivery-lifetime contract for PreviewService / WebcamService PostFrame: queued
# frames are dropped once the bound receiver QObject is destroyed, so the callback
# is never invoked against a freed page (the latent use-after-free fix). Drives the
# protected PostFrame directly -- no camera, no WGC. WebcamService is provided by
# the device-service support lib; PreviewService is compiled in for its D3D/WGC bits.
exosnap_add_gtest(
    NAME service_frame_delivery_tests
    TEST_PREFIX service.
    SOURCES
        tests/test_service_frame_delivery.cpp
        services/PreviewService.cpp
    LIBRARIES
        exosnap_device_service_test_support
        Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
        d3d11.lib dxgi.lib runtimeobject.lib ole32.lib)
target_include_directories(service_frame_delivery_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(service_frame_delivery_tests PRIVATE NOMINMAX)
set_target_properties(service_frame_delivery_tests PROPERTIES AUTOMOC ON)

# Sharing and disposal: one hub per source key, born with the first consumer and
# discarded with the last. Same fake-producer seam, no GPU.
exosnap_add_gtest(
    NAME capture_hub_registry_tests
    TEST_PREFIX capture.
    SOURCES
        tests/test_capture_hub_registry.cpp
        services/CaptureHubRegistry.cpp
        services/CaptureSourceHub.cpp
    LIBRARIES recorder_core)
target_include_directories(capture_hub_registry_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Which mip a tile is read back from. Pure arithmetic; no D3D, no Qt.
exosnap_add_gtest(
    NAME thumbnail_mip_tests
    TEST_PREFIX capture.
    SOURCES tests/test_thumbnail_mip.cpp)
target_include_directories(thumbnail_mip_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The sequential "Display N" label shared by picker, Record header and output
# filename. Pure given the sequence map; no Qt.
exosnap_add_gtest(
    NAME display_numbering_tests
    TEST_PREFIX record.
    SOURCES
        tests/test_display_numbering.cpp
        services/DisplayNumbering.cpp)
target_include_directories(display_numbering_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME display_device_notifier_tests
    TEST_PREFIX device.
    SOURCES
        tests/test_display_device_notifier.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(display_device_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(display_device_notifier_tests PRIVATE ole32.lib)
set_target_properties(display_device_notifier_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME capture_target_notifier_tests
    TEST_PREFIX capture.targets.
    SOURCES
        tests/test_capture_target_notifier.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(capture_target_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(capture_target_notifier_tests PRIVATE ole32.lib user32.lib)
set_target_properties(capture_target_notifier_tests PROPERTIES AUTOMOC ON)

# Selection-preservation policy tests (Worker C).
# Drives page methods with synthetic snapshots at the integration level.
exosnap_add_gtest(
    NAME device_selection_policy_tests
    TEST_PREFIX discovery.
    SOURCES
        tests/test_device_selection_policy.cpp
    LIBRARIES exosnap_pages_test_support Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(device_selection_policy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(device_selection_policy_tests PROPERTIES AUTOMOC ON)
target_link_libraries(device_selection_policy_tests PRIVATE
    mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib ole32.lib)

# CAPTURE-FRAME-R1 tests
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    exosnap_add_gtest(
        NAME capture_frame_tests
        TEST_PREFIX capture.frame.
        SOURCES
            tests/test_capture_frame.cpp
            services/RecordingCoordinator.cpp
            services/AtomicFileOps.cpp
            services/DisplayNumbering.cpp
            settings/RecoveryManifestStore.cpp
            services/GlobalHotkeyService.cpp
            visual_tests/VisualScenario.cpp
        LIBRARIES
            exosnap_output_model_test_support
            exosnap_device_service_test_support
            exosnap_diagnostics_test_support
            Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
    )
    target_include_directories(capture_frame_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(capture_frame_tests PRIVATE
        NOMINMAX EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1)
    # ADR-0014: mfreadwrite.lib removed (MF/SinkWriter path deleted).
    target_link_libraries(capture_frame_tests PRIVATE
        mfplat.lib mf.lib mfuuid.lib ole32.lib)
    set_target_properties(capture_frame_tests PROPERTIES AUTOMOC ON)
endif()

# RECORDING-OVERLAY-R1 tests
exosnap_add_gtest(
    NAME recording_overlay_tests
    TEST_PREFIX overlay.
    SOURCES
        tests/test_recording_overlay.cpp
        ui/overlay/RecordingOverlayWindow.cpp
        settings/AppSettingsStore.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(recording_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(recording_overlay_tests PRIVATE NOMINMAX)
set_target_properties(recording_overlay_tests PROPERTIES AUTOMOC ON)

# COUNTDOWN-OVERLAY-R1 tests
exosnap_add_gtest(
    NAME countdown_overlay_tests
    TEST_PREFIX overlay.
    SOURCES
        tests/test_countdown_overlay.cpp
        ui/overlay/CountdownOverlayWindow.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(countdown_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(countdown_overlay_tests PRIVATE NOMINMAX)
set_target_properties(countdown_overlay_tests PROPERTIES AUTOMOC ON)

# DIAGNOSTICS-OVERLAY-R1 tests
exosnap_add_gtest(
    NAME diagnostics_overlay_tests
    TEST_PREFIX overlay.
    SOURCES
        tests/test_diagnostics_overlay.cpp
        ui/overlay/DiagnosticsOverlayWindow.cpp
        settings/AppSettingsStore.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(diagnostics_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(diagnostics_overlay_tests PRIVATE NOMINMAX)
set_target_properties(diagnostics_overlay_tests PROPERTIES AUTOMOC ON)

# QUICK-PILL-R1 tests
exosnap_add_gtest(
    NAME quick_control_pill_tests
    TEST_PREFIX overlay.
    SOURCES
        tests/test_quick_control_pill.cpp
        ui/overlay/QuickControlPillWindow.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(quick_control_pill_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(quick_control_pill_tests PRIVATE NOMINMAX)
set_target_properties(quick_control_pill_tests PROPERTIES AUTOMOC ON)

# NOTIFY-TOASTS-R1 tests
exosnap_add_gtest(
    NAME notification_manager_tests
    TEST_PREFIX notify.
    SOURCES
        tests/test_notification_manager.cpp
        notifications/NotificationManager.cpp
    LIBRARIES Qt6::Core exosnap_capability recorder_core
)
target_include_directories(notification_manager_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(notification_manager_tests PROPERTIES AUTOMOC ON)

# CAPTURE-FRAME-DOCK-BUTTON-R1 tests
# TransportDock and AudioSourceToggle included so tests 11-13 (dock button
# seams/visibility/click) can exercise the live widget without requiring the
# full RecordPage.  CountdownSelect removed: replaced by split-button + QMenu.
exosnap_add_gtest(
    NAME capture_frame_button_tests
    TEST_PREFIX capture.frame.button.
    SOURCES
        tests/test_capture_frame_button.cpp
        services/RecordingCoordinator.cpp
        services/AtomicFileOps.cpp
        services/DisplayNumbering.cpp
        settings/RecoveryManifestStore.cpp
        notifications/NotificationManager.cpp
        ui/widgets/TransportDock.cpp
        ui/widgets/AudioSourceToggle.cpp
        # THEME-SLICE-2C: AudioSourceToggle now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES
        exosnap_output_model_test_support
        exosnap_device_service_test_support
        exosnap_diagnostics_test_support
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core
)
target_include_directories(capture_frame_button_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(capture_frame_button_tests PRIVATE NOMINMAX)
target_link_libraries(capture_frame_button_tests PRIVATE mfplat.lib mf.lib mfuuid.lib ole32.lib)
set_target_properties(capture_frame_button_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME notification_toast_tests
    TEST_PREFIX notify.
    SOURCES
        tests/test_notification_toast.cpp
        notifications/NotificationManager.cpp
        ui/overlay/NotificationToastWindow.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(notification_toast_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(notification_toast_tests PRIVATE NOMINMAX)
set_target_properties(notification_toast_tests PROPERTIES AUTOMOC ON)

# OVERLAY-SKIN-AND-PROOF-R1: offscreen visual-proof renders for capture-excluded overlay windows.
# Uses QWidget::grab() which bypasses WDA_EXCLUDEFROMCAPTURE; produces PNGs under
# .workspace/screenshots/overlay-proof/ for human visual verification.
# REGION-SELECTION-SKIN-R1: adds region-selection.png proof.
exosnap_add_gtest(
    NAME overlay_visual_proof_tests
    TEST_PREFIX overlay.proof.
    SOURCES
        tests/overlay_visual_proof.cpp
        notifications/NotificationManager.cpp
        ui/overlay/CountdownOverlayWindow.cpp
        ui/overlay/NotificationToastWindow.cpp
        ui/overlay/QuickControlPillWindow.cpp
        ui/overlay/RecordingOverlayWindow.cpp
        ui/overlay/DiagnosticsOverlayWindow.cpp
        ui/widgets/RegionSelectionOverlay.cpp
        ui/widgets/RegionGeometry.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(overlay_visual_proof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(overlay_visual_proof_tests PRIVATE NOMINMAX)
set_target_properties(overlay_visual_proof_tests PROPERTIES AUTOMOC ON)

# CRASH-UPDATE-VISUAL-PROOF-R1: offscreen renders of the 0.4.0 crash + update UI
# surfaces. Applies the real ExoSnap theme (QSS + bundled fonts), so the theme
# and fonts qrc resources are compiled in via AUTORCC. Produces 7 PNGs under
# .workspace/screenshots/0.4.0/ for human visual verification.
exosnap_add_gtest(
    NAME crash_update_visual_proof_tests
    TEST_PREFIX crash.update.proof.
    SOURCES
        tests/crash_update_visual_proof.cpp
        ui/dialogs/CrashReportOverlay.cpp
        ui/dialogs/CrashReportPanel.cpp
        ui/brand/BrandMarkWidget.cpp
        ui/theme/LucideIcon.cpp
        ui/theme/ExoSnapTheme.cpp
        # DESIGN-SYNC-R1: CrashReportPanel's remember-choice draft is an ExoCheckBox.
        ui/widgets/ExoCheckBox.cpp
        ui/theme/exosnap_theme.qrc
        assets/fonts/exosnap_fonts.qrc
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(crash_update_visual_proof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(crash_update_visual_proof_tests PRIVATE NOMINMAX)
set_target_properties(crash_update_visual_proof_tests PROPERTIES AUTOMOC ON AUTORCC ON)

exosnap_add_gtest(
    NAME whats_new_visual_proof_tests
    TEST_PREFIX whats.new.proof.
    SOURCES
        tests/whats_new_visual_proof.cpp
        ui/dialogs/WhatsNewOverlay.cpp
        ui/widgets/ExoCheckBox.cpp
        ui/theme/LucideIcon.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/exosnap_theme.qrc
        assets/fonts/exosnap_fonts.qrc
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(whats_new_visual_proof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(whats_new_visual_proof_tests PRIVATE NOMINMAX)
set_target_properties(whats_new_visual_proof_tests PROPERTIES AUTOMOC ON AUTORCC ON)

# RECORDING-ERROR-MODAL-R1: offscreen renders of the non-fatal recording-error
# dialog. Applies the real ExoSnap theme; produces PNGs under
# .workspace/screenshots/0.7.0/ for human visual verification.
exosnap_add_gtest(
    NAME recording_error_visual_proof_tests
    TEST_PREFIX record.error.proof.
    SOURCES
        tests/recording_error_visual_proof.cpp
        ui/dialogs/RecordingErrorPanel.cpp
        ui/theme/LucideIcon.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/exosnap_theme.qrc
        assets/fonts/exosnap_fonts.qrc
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(recording_error_visual_proof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(recording_error_visual_proof_tests PRIVATE NOMINMAX)
set_target_properties(recording_error_visual_proof_tests PROPERTIES AUTOMOC ON AUTORCC ON)

# REGION-SELECTION-SKIN-R1 unit tests.
exosnap_add_gtest(
    NAME region_selection_overlay_tests
    TEST_PREFIX region.overlay.
    SOURCES
        tests/test_region_selection_overlay.cpp
        ui/widgets/RegionSelectionOverlay.cpp
        ui/widgets/RegionGeometry.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(region_selection_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(region_selection_overlay_tests PRIVATE NOMINMAX)
set_target_properties(region_selection_overlay_tests PROPERTIES AUTOMOC ON)

# RECORDING-MARKERS-R1 tests
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    exosnap_add_gtest(
        NAME recording_marker_tests
        TEST_PREFIX marker.
        SOURCES
            tests/test_recording_markers.cpp
            services/RecordingCoordinator.cpp
            services/AtomicFileOps.cpp
            services/DisplayNumbering.cpp
            settings/RecoveryManifestStore.cpp
            services/GlobalHotkeyService.cpp
            visual_tests/VisualScenario.cpp
        LIBRARIES
            exosnap_output_model_test_support
            exosnap_device_service_test_support
            exosnap_diagnostics_test_support
            Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
    )
    target_include_directories(recording_marker_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(recording_marker_tests PRIVATE
        NOMINMAX EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1)
    target_link_libraries(recording_marker_tests PRIVATE mfplat.lib mf.lib mfuuid.lib ole32.lib)
    set_target_properties(recording_marker_tests PROPERTIES AUTOMOC ON)
endif()

# TRAY-PRESENCE-R1 tests
exosnap_add_gtest(
    NAME tray_presence_tests
    TEST_PREFIX tray.
    SOURCES
        tests/test_tray_presence.cpp
        ui/tray/TrayPresence.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(tray_presence_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(tray_presence_tests PROPERTIES AUTOMOC ON)

# Pure unit tests for HumanizeEngineDetail (RecordingErrorPanel's raw-enum → canon
# label mapping). Dependency-light: QString only.
exosnap_add_gtest(
    NAME recording_error_detail_text_tests
    TEST_PREFIX record.error.detail.
    SOURCES
        tests/test_recording_error_detail_text.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(recording_error_detail_text_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(recording_error_detail_text_tests PROPERTIES AUTOMOC ON)

# Offscreen render of the startup Recovery card. Applies the real ExoSnap theme;
# produces .workspace/screenshots/recovery-overlay.png for visual verification.
exosnap_add_gtest(
    NAME recovery_overlay_visual_proof_tests
    TEST_PREFIX recovery.proof.
    SOURCES
        tests/recovery_overlay_visual_proof.cpp
        ui/dialogs/RecoveryOverlay.cpp
        settings/RecoveryManifestStore.cpp
        services/RecoveryService.cpp
        services/AtomicFileOps.cpp
        diagnostics/AppLog.cpp
        ui/brand/BrandMarkWidget.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/exosnap_theme.qrc
        assets/fonts/exosnap_fonts.qrc
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core FFmpeg::mux
)
target_include_directories(recovery_overlay_visual_proof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(recovery_overlay_visual_proof_tests PRIVATE NOMINMAX)
set_target_properties(recovery_overlay_visual_proof_tests PROPERTIES AUTOMOC ON AUTORCC ON)

# Guard: every curated theme (ExoSnapThemes.h) must resolve every `${token}` in
# the real compiled QSS resource. An invalid/orphaned token crashes at startup
# in Debug (Q_ASSERT_X in ExoSnapTheme.cpp) and ships a literal "${token}"
# string in Release; this drives the real ApplyExoSnapTheme()/ReapplyTheme()
# path for every shipped theme so a QSS edit can't silently leave one broken.
exosnap_add_gtest(
    NAME theme_token_resolution_tests
    TEST_PREFIX theme.
    SOURCES
        tests/test_theme_token_resolution.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/exosnap_theme.qrc
        assets/fonts/exosnap_fonts.qrc
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(theme_token_resolution_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(theme_token_resolution_tests PRIVATE NOMINMAX)
set_target_properties(theme_token_resolution_tests PROPERTIES AUTOMOC ON AUTORCC ON)

# Pixel proof that ReapplyTheme() restyles widgets whose inline stylesheets /
# tinted pixmaps derive from ActiveTheme() (the CompareHint popover was the
# confirmed stuck-dark case), and that a dark → light → dark round trip renders
# byte-identically. Writes PNGs to .workspace/screenshots/theme-switch/.
exosnap_add_gtest(
    NAME theme_switch_visual_proof_tests
    TEST_PREFIX theme.
    SOURCES
        tests/theme_switch_visual_proof.cpp
        ui/widgets/CompareHint.cpp
        models/SettingsCompareData.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
        ui/theme/exosnap_theme.qrc
        assets/fonts/exosnap_fonts.qrc
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(theme_switch_visual_proof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(theme_switch_visual_proof_tests PRIVATE NOMINMAX)
set_target_properties(theme_switch_visual_proof_tests PROPERTIES AUTOMOC ON AUTORCC ON)

# Titlebar window-button hover/click-target proof with the *real* shipping theme
# applied (QSS + bundled fonts) — the plain operational_title_bar_tests target has
# no stylesheet loaded at all, so it cannot see stylesheet-only regressions (a
# generic QPushButton rule's min-height/padding, combined with the buttons' own
# margin, silently grew the real widget past its click target and separately
# produced a degenerate paint rect). Writes PNGs under
# .workspace/screenshots/titlebar-window-buttons/.
exosnap_add_gtest(
    NAME titlebar_window_buttons_visual_proof_tests
    TEST_PREFIX chrome.proof.
    SOURCES
        tests/titlebar_window_buttons_visual_proof.cpp
        ui/chrome/OperationalTitleBar.cpp
        ui/brand/BrandMarkWidget.cpp
        ui/widgets/NotificationBell.cpp
        ui/widgets/StatusPill.cpp
        ui/theme/LucideIcon.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/exosnap_theme.qrc
        assets/fonts/exosnap_fonts.qrc
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core
)
target_include_directories(titlebar_window_buttons_visual_proof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(titlebar_window_buttons_visual_proof_tests PRIVATE NOMINMAX)
set_target_properties(titlebar_window_buttons_visual_proof_tests PROPERTIES AUTOMOC ON AUTORCC ON)

# ---------------------------------------------------------------------------
# Install / windeployqt
# ---------------------------------------------------------------------------
if(WIN32 AND Qt6_FOUND)
    qt_generate_deploy_app_script(
        TARGET exosnap
        OUTPUT_SCRIPT _exosnap_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )

    # Root project license, third-party notices, and release-facing portable
    # documentation (before TARGETS so they install even if the exe target is
    # missing during dev iteration). These are the only docs shipped in the
    # portable artifact; CMake install is the sole authority for the tree.
    install(
        FILES
            "${PROJECT_SOURCE_DIR}/LICENSE"
            "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "${PROJECT_SOURCE_DIR}/KNOWN_LIMITATIONS.md"
            "${PROJECT_SOURCE_DIR}/README-PORTABLE.md"
        DESTINATION "."
    )

    # Qt LGPLv3 license text staged with a clean name alongside other
    # third-party licenses. The staging directory is shared with
    # third_party/CMakeLists.txt so the existing install(DIRECTORY) rule
    # picks up this file automatically.
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/license_staging")
    configure_file(
        "${PROJECT_SOURCE_DIR}/third_party/qt/LGPL-3.0.txt"
        "${PROJECT_BINARY_DIR}/license_staging/qt.txt"
        COPYONLY
    )

    install(TARGETS exosnap RUNTIME DESTINATION .)

    # Deploy the Qt runtime (DLLs + qt.conf) flat next to exosnap.exe rather than
    # into a bin/ subdirectory. exosnap.exe installs to the prefix root, and a
    # root executable on Windows cannot load Qt DLLs from bin/ — a flat layout is
    # required for the portable package to launch. Qt's deploy support honors a
    # pre-set QT_DEPLOY_BIN_DIR (it only defaults the value when undefined), and
    # this install(CODE) runs in the same cmake_install.cmake pass as the deploy
    # script below, so the override is in effect when the script runs.
    install(CODE "set(QT_DEPLOY_BIN_DIR \".\")")
    install(SCRIPT ${_exosnap_deploy_script})
endif()

# ---------------------------------------------------------------------------
# Qt Quick frontend
# ---------------------------------------------------------------------------
# Added last, not next to the find_package() above: the Quick target compiles the
# shared VERSIONINFO resource generated by configure_file() further up this file
# and depends on targets (exosnap_build_info, the crash/update libraries) that
# only exist by this point. Processing the subdirectory earlier left both as
# forward references that happened to resolve at generate time.
if(EXOSNAP_BUILD_QUICK_SPIKE)
    add_subdirectory(quick/ExoSnap/Quick)
endif()
