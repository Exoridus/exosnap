cmake_minimum_required(VERSION 3.27)

#Local Qt install hint — override via Qt6_DIR or CMAKE_PREFIX_PATH.
if(NOT Qt6_DIR)
    list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.9.0/msvc2022_64")
endif()

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Svg)

if(NOT Qt6_FOUND)
    add_custom_target(exosnap COMMAND ${CMAKE_COMMAND} -E echo "exosnap skipped: Qt6 not found")
    message(WARNING "Qt6 not found — exosnap app will not be built. Install Qt 6 and set Qt6_DIR or CMAKE_PREFIX_PATH.")
    return()
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
# Single source of truth: derived from the root project(... VERSION ...) call.
# Never hard-code the version string here.
set(EXOSNAP_VERSION "${PROJECT_VERSION}")

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

set(EXOSNAP_BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/ExoSnapBuildInfo.h")
add_custom_target(exosnap_build_info
    ALL
    COMMAND
        ${CMAKE_COMMAND}
        -DEXOSNAP_VERSION=${EXOSNAP_VERSION}
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
    ui/theme/ExoSnapAccents.h
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
    ui/dialogs/AboutOverlay.h
    ui/dialogs/AboutOverlay.cpp
    ui/dialogs/PresetManageOverlay.h
    ui/dialogs/PresetManageOverlay.cpp
    ui/dialogs/UpdateSettingsPanel.h
    ui/dialogs/UpdateSettingsPanel.cpp
    ui/dialogs/RecoveryOverlay.h
    ui/dialogs/RecoveryOverlay.cpp
    ui/dialogs/SourcePickerPanel.h
    ui/dialogs/SourcePickerPanel.cpp
    ui/dialogs/SourcePickerDialog.h
    ui/dialogs/SourcePickerDialog.cpp
    ui/dialogs/SourcePickerOverlay.h
    ui/dialogs/SourcePickerOverlay.cpp
    ui/dialogs/SourcePickerWindowRules.h
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
    ui/widgets/WebcamSetupPanel.h
    ui/widgets/WebcamSetupPanel.cpp
    ui/widgets/CaptureTargetCard.h
    ui/widgets/CaptureTargetCard.cpp
    ui/widgets/RegionPresetCard.h
    ui/widgets/RegionPresetCard.cpp
    ui/widgets/PipelineStepCard.h
    ui/widgets/PipelineStepCard.cpp
    ui/widgets/PipelineFlow.h
    ui/widgets/PipelineFlow.cpp
    ui/widgets/LivePipelinePanel.h
    ui/widgets/LivePipelinePanel.cpp
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
    ui/widgets/SettingsPopoverRow.h
    ui/widgets/SettingsPopoverRow.cpp
    ui/widgets/NotificationBell.h
    ui/widgets/NotificationBell.cpp
    ui/widgets/AdvisoryItem.h
    ui/widgets/AdvisoryItem.cpp
    ui/widgets/CornerCaptureButton.h
    ui/widgets/CornerCaptureButton.cpp
    ui/widgets/KeyCap.h
    ui/widgets/KeyCap.cpp
    ui/widgets/PlaceholderRow.h
    ui/widgets/PlaceholderRow.cpp
    ui/widgets/TransportDock.h
    ui/widgets/TransportDock.cpp
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
    pages/RecordPage.h
    pages/RecordPage.cpp
    pages/ConfigPage.h
    pages/ConfigPage.cpp
    pages/HotkeysPage.h
    pages/HotkeysPage.cpp
    pages/WebcamPage.h
    pages/WebcamPage.cpp
    pages/DiagnosticsPage.h
    pages/DiagnosticsPage.cpp
    pages/LogsPage.h
    pages/LogsPage.cpp
    pages/OutputPage.h
    pages/OutputPage.cpp
    pages/EditExportPage.h
    pages/EditExportPage.cpp
    services/DeviceDiscoveryCommon.h
    services/AudioDeviceNotifier.h
    services/AudioDeviceNotifier.cpp
    services/WebcamDeviceNotifier.h
    services/WebcamDeviceNotifier.cpp
    services/DisplayDeviceNotifier.h
    services/DisplayDeviceNotifier.cpp
    services/GlobalHotkeyService.h
    services/GlobalHotkeyService.cpp
    services/PreviewService.h
    services/PreviewService.cpp
    services/PreviewHelpers.h
    services/ThumbnailCapture.h
    services/ThumbnailCapture.cpp
    services/DxgiPreviewRenderer.h
    services/DxgiPreviewRenderer.cpp
    services/WebcamService.h
    services/WebcamService.cpp
    services/RecordingCoordinator.h
    services/RecordingCoordinator.cpp
    services/RecordingCountdownController.h
    services/RecordingCountdownController.cpp
    services/RecoveryService.h
    services/RecoveryService.cpp
    services/CrashIssueReport.h
    services/CrashIssueReport.cpp
    services/UpdateService.h
    services/UpdateService.cpp
    models/WebcamSettings.h
    models/SettingsCompareData.h
    models/SettingsCompareData.cpp
    models/OutputSettingsModel.h
    models/OutputSettingsModel.cpp
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
    settings/AppSettingsStore.h
    settings/AppSettingsStore.cpp
    settings/RecordingPresetStore.h
    settings/RecordingPresetStore.cpp
    settings/RecordingHistoryStore.h
    settings/RecordingHistoryStore.cpp
    settings/RecoveryManifestStore.h
    settings/RecoveryManifestStore.cpp
    diagnostics/error_message.h
    diagnostics/error_message.cpp
    diagnostics/AppLog.h
    diagnostics/AppLog.cpp
    diagnostics/DiagnosticResult.h
    diagnostics/DiagnosticResult.cpp
    diagnostics/CapabilitySummary.h
    diagnostics/CapabilitySummary.cpp
    diagnostics/ConfigSummary.h
    diagnostics/ConfigSummary.cpp
    diagnostics/DiskSpaceProvider.h
    diagnostics/DiskSpaceProvider.cpp
    diagnostics/DiskSpaceThresholds.h
    diagnostics/FilesystemProvider.h
    diagnostics/FilesystemProvider.cpp
    diagnostics/RecommendationEngine.h
    diagnostics/RecommendationEngine.cpp
    diagnostics/SelfTestRunner.h
    diagnostics/SelfTestRunner.cpp
    viewmodels/RecordViewModel.h
    viewmodels/RecordViewModel.cpp
    viewmodels/PresentationState.h
    viewmodels/PresentationStateBuilder.h
    viewmodels/PresentationStateBuilder.cpp
)

target_sources(exosnap PRIVATE
    "$<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}/visual_tests/VisualScenario.h>"
    "$<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}/visual_tests/VisualScenario.cpp>"
    "$<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}/visual_tests/VisualTestHarness.h>"
    "$<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}/visual_tests/VisualTestHarness.cpp>"
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
target_compile_definitions(exosnap PRIVATE
    NOMINMAX
    EXOSNAP_BUILD_CONFIG="$<CONFIG>"
    "$<$<NOT:$<CONFIG:Release>>:EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1>"
)
add_dependencies(exosnap exosnap_build_info)

target_link_libraries(exosnap PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
    recorder_core exosnap::capability exosnap::warnings FFmpeg::mux exosnap::crash_capture
    exosnap::update
    tomlplusplus::tomlplusplus
    d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib
    mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib
)

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
    diagnostics/SelfTestRunner.cpp
)
target_link_libraries(exosnap_diagnostics_test_support PUBLIC
    exosnap_output_model_test_support exosnap_capability recorder_core
)

exosnap_add_app_test_support(exosnap_device_service_test_support
    services/AudioDeviceNotifier.cpp
    services/WebcamDeviceNotifier.cpp
    services/DisplayDeviceNotifier.cpp
    services/WebcamService.cpp
)
target_link_libraries(exosnap_device_service_test_support PUBLIC
    exosnap_app_log_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core
    ole32.lib mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib
)

exosnap_add_app_test_support(exosnap_pages_test_support
    pages/ConfigPage.cpp
    pages/WebcamPage.cpp
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
    # UPDATE-WIRE-R1: ConfigPage now embeds the UpdateSettingsPanel, which pulls in
    # the Lucide glyph renderer.
    ui/dialogs/UpdateSettingsPanel.cpp
    ui/theme/LucideIcon.cpp
    # PS-PHASE-C: HotkeysSettingsPanel embedded in ConfigPage.
    ui/widgets/HotkeysSettingsPanel.cpp
    ui/widgets/KeycapChip.cpp
    pages/HotkeysPage.cpp
    services/GlobalHotkeyService.cpp
    ui/widgets/SectionRuleHeader.cpp
    ui/widgets/PlaceholderRow.cpp
    # THEME-SLICE-1: ExoCheckBox now calls ActiveTheme()/ParseThemeColor() from ExoSnapTheme.cpp.
    ui/theme/ExoSnapTheme.cpp
    # S3: ExoSlider used by ConfigPage (mic gain slider with gradient groove + ticks).
    ui/widgets/ExoSlider.cpp
    # S5: SettingsPopoverRow used by ConfigPage for mic post-processing / limiter / split.
    ui/widgets/SettingsPopoverRow.cpp
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
    # THEME-SLICE-2C: SourcePickerOverlay now calls ActiveTheme() from ExoSnapTheme.cpp.
    ui/theme/ExoSnapTheme.cpp
)
target_link_libraries(exosnap_source_picker_test_support PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets d3d11.lib dxgi.lib runtimeobject.lib ole32.lib
)

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

exosnap_add_gtest(
    NAME output_settings_tests
    TEST_PREFIX output.
    SOURCES
        tests/test_output_settings.cpp
        services/RecordingCoordinator.cpp
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

# MP4-SPLIT-REMUX-R1: per-segment manifest lifecycle, disk reserve, split settings.
exosnap_add_gtest(
    NAME mp4_split_remux_tests
    TEST_PREFIX split_remux.
    SOURCES
        tests/test_mp4_split_remux.cpp
        services/RecordingCoordinator.cpp
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

# ACCENT-PICKER-R1 (0.5.0-B): accent id resolution and table invariant tests.
# Headless — no QApplication required; only inspects ExoSnapAccents + ExoSnapPalette.
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
    NAME key_cap_tests
    TEST_PREFIX suite.
    SOURCES
        tests/test_key_cap.cpp
        ui/widgets/KeyCap.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets
)
target_include_directories(key_cap_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(key_cap_tests PROPERTIES AUTOMOC ON)

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
    NAME webcam_page_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_page.cpp
    LIBRARIES exosnap_pages_test_support Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(webcam_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(webcam_page_tests PROPERTIES AUTOMOC ON)
target_link_libraries(webcam_page_tests PRIVATE mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib ole32.lib)

exosnap_add_gtest(
    NAME hotkeys_page_tests
    TEST_PREFIX hotkey.
    SOURCES
        tests/test_hotkeys_page.cpp
        pages/HotkeysPage.cpp
        services/GlobalHotkeyService.cpp
        ui/widgets/SectionRuleHeader.cpp
        ui/widgets/KeycapChip.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(hotkeys_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(hotkeys_page_tests PROPERTIES AUTOMOC ON)

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
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(logs_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(logs_page_tests PRIVATE EXOSNAP_ENABLE_VISUAL_TEST_HARNESS=1)
set_target_properties(logs_page_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME about_overlay_tests
    TEST_PREFIX about.
    SOURCES
        tests/test_about_overlay.cpp
        ui/dialogs/AboutOverlay.cpp
        ui/brand/BrandMarkWidget.cpp
        # AboutOverlay keeps a hidden UpdateSettingsPanel for MainWindow wiring.
        ui/dialogs/UpdateSettingsPanel.cpp
        ui/theme/LucideIcon.cpp
        # THEME-SLICE-1: BrandMarkWidget now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(about_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(about_overlay_tests PRIVATE EXOSNAP_BUILD_CONFIG="$<CONFIG>")
add_dependencies(about_overlay_tests exosnap_build_info)
set_target_properties(about_overlay_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME edit_export_page_tests
    TEST_PREFIX edit_export.
    SOURCES
        tests/test_edit_export_page.cpp
        pages/EditExportPage.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(edit_export_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(edit_export_page_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME preset_manage_overlay_tests
    TEST_PREFIX preset_manage.
    SOURCES
        tests/test_preset_manage_overlay.cpp
        ui/dialogs/PresetManageOverlay.cpp
        models/RecordingPreset.cpp
        models/RecordingPresetRegistry.cpp
        models/OutputSettingsModel.cpp
        # THEME-SLICE-2C: PresetManageOverlay now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(preset_manage_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(preset_manage_overlay_tests PRIVATE NOMINMAX)
target_link_libraries(preset_manage_overlay_tests PRIVATE ole32.lib shell32.lib)
set_target_properties(preset_manage_overlay_tests PROPERTIES AUTOMOC ON)

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
        # DESIGN-SYNC-R1: CrashReportPanel's auto-send checkbox is now ExoCheckBox.
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
    NAME crash_issue_report_tests
    TEST_PREFIX crashissue.
    SOURCES
        tests/test_crash_issue_report.cpp
        services/CrashIssueReport.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(crash_issue_report_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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

exosnap_add_gtest(
    NAME update_settings_panel_tests
    TEST_PREFIX update_ui.
    SOURCES
        tests/test_update_settings_panel.cpp
        ui/dialogs/UpdateSettingsPanel.cpp
        ui/theme/ExoSnapTheme.cpp
        ui/theme/LucideIcon.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(update_settings_panel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(update_settings_panel_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME version_tests
    TEST_PREFIX version.
    SOURCES
        tests/test_version.cpp
)
target_include_directories(version_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(version_tests PRIVATE EXOSNAP_EXPECTED_VERSION="${PROJECT_VERSION}")
add_dependencies(version_tests exosnap_build_info)

exosnap_add_gtest(
    NAME preview_helpers_tests
    TEST_PREFIX preview.
    SOURCES
        tests/test_preview_helpers.cpp
    LIBRARIES recorder_core
)
target_include_directories(preview_helpers_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
    LIBRARIES
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg exosnap_capability recorder_core
        d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib
)
target_include_directories(preview_surface_webcam_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(preview_surface_webcam_tests PRIVATE NOMINMAX)
set_target_properties(preview_surface_webcam_tests PROPERTIES AUTOMOC ON)

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
    LIBRARIES exosnap_diagnostics_test_support Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(diagnostics_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(diagnostics_page_tests PROPERTIES AUTOMOC ON)

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

exosnap_add_gtest(
    NAME recovery_service_tests
    TEST_PREFIX recovery.
    SOURCES tests/test_recovery_service.cpp
            settings/RecoveryManifestStore.cpp
            services/RecoveryService.cpp
            diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core FFmpeg::mux
)
target_include_directories(recovery_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(recovery_service_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME recovery_overlay_tests
    TEST_PREFIX recovery.
    SOURCES
        tests/test_recovery_overlay.cpp
        ui/dialogs/RecoveryOverlay.cpp
        settings/RecoveryManifestStore.cpp
        services/RecoveryService.cpp
        diagnostics/AppLog.cpp
        # THEME-SLICE-2C: RecoveryOverlay now calls ActiveTheme() from ExoSnapTheme.cpp.
        ui/theme/ExoSnapTheme.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core FFmpeg::mux
)
target_include_directories(recovery_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(recovery_overlay_tests PRIVATE NOMINMAX)
set_target_properties(recovery_overlay_tests PROPERTIES AUTOMOC ON)

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

exosnap_add_gtest(
    NAME display_device_notifier_tests
    TEST_PREFIX device.
    SOURCES
        tests/test_display_device_notifier.cpp
    LIBRARIES exosnap_device_service_test_support Qt6::Core Qt6::Gui exosnap_capability recorder_core)
target_include_directories(display_device_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(display_device_notifier_tests PRIVATE ole32.lib)
set_target_properties(display_device_notifier_tests PROPERTIES AUTOMOC ON)

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
# and fonts qrc resources are compiled in via AUTORCC. Produces 6 PNGs under
# .workspace/screenshots/0.4.0/ for human visual verification.
exosnap_add_gtest(
    NAME crash_update_visual_proof_tests
    TEST_PREFIX crash.update.proof.
    SOURCES
        tests/crash_update_visual_proof.cpp
        ui/dialogs/CrashReportPanel.cpp
        ui/dialogs/UpdateSettingsPanel.cpp
        ui/brand/BrandMarkWidget.cpp
        ui/theme/LucideIcon.cpp
        ui/theme/ExoSnapTheme.cpp
        # DESIGN-SYNC-R1: CrashReportPanel's auto-send checkbox is now ExoCheckBox.
        ui/widgets/ExoCheckBox.cpp
        ui/theme/exosnap_theme.qrc
        assets/fonts/exosnap_fonts.qrc
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
)
target_include_directories(crash_update_visual_proof_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(crash_update_visual_proof_tests PRIVATE NOMINMAX)
set_target_properties(crash_update_visual_proof_tests PROPERTIES AUTOMOC ON AUTORCC ON)

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
