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
    ui/theme/exosnap_theme.qrc
    ui/brand/BrandMarkWidget.h
    ui/brand/BrandMarkWidget.cpp
    ui/WindowGeometryPolicy.h
    ui/WindowGeometryPolicy.cpp
    ui/chrome/OperationalTitleBar.h
    ui/chrome/OperationalTitleBar.cpp
    ui/chrome/GlobalRecordingBar.h
    ui/chrome/GlobalRecordingBar.cpp
    ui/dialogs/AboutOverlay.h
    ui/dialogs/AboutOverlay.cpp
    ui/dialogs/RecoveryOverlay.h
    ui/dialogs/RecoveryOverlay.cpp
    ui/dialogs/SourcePickerPanel.h
    ui/dialogs/SourcePickerPanel.cpp
    ui/dialogs/SourcePickerDialog.h
    ui/dialogs/SourcePickerDialog.cpp
    ui/dialogs/SourcePickerOverlay.h
    ui/dialogs/SourcePickerOverlay.cpp
    ui/dialogs/SourcePickerWindowRules.h
    ui/tray/TrayPresence.h
    ui/tray/TrayPresence.cpp
    ui/widgets/SectionRuleHeader.h
    ui/widgets/SectionRuleHeader.cpp
    ui/widgets/KeycapChip.h
    ui/widgets/KeycapChip.cpp
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
    ui/widgets/CountdownSelect.h
    ui/widgets/CountdownSelect.cpp
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
    pages/AdvancedPage.h
    pages/AdvancedPage.cpp
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
    models/WebcamSettings.h
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
    recorder_core exosnap::capability exosnap::warnings FFmpeg::mux
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
    ui/widgets/ExoToggle.cpp
    ui/widgets/VUMeterWidget.cpp
    ui/widgets/WebcamSetupPanel.cpp
)
target_link_libraries(exosnap_pages_test_support PUBLIC
    exosnap_device_service_test_support exosnap_output_model_test_support
    Qt6::Core Qt6::Gui Qt6::Widgets
)

exosnap_add_app_test_support(exosnap_source_picker_test_support
    ui/dialogs/SourcePickerPanel.cpp
    ui/dialogs/SourcePickerDialog.cpp
    ui/dialogs/SourcePickerOverlay.cpp
    ui/widgets/RegionGeometry.cpp
    ui/widgets/CaptureTargetCard.cpp
    ui/widgets/RegionPresetCard.cpp
    services/ThumbnailCapture.cpp
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
        ui/widgets/StatusPill.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
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
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets
)
target_include_directories(about_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(about_overlay_tests PRIVATE EXOSNAP_BUILD_CONFIG="$<CONFIG>")
add_dependencies(about_overlay_tests exosnap_build_info)
set_target_properties(about_overlay_tests PROPERTIES AUTOMOC ON)

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
    NAME advanced_page_tests
    TEST_PREFIX advanced.
    SOURCES
        tests/test_advanced_page.cpp
        pages/AdvancedPage.cpp
        ui/widgets/SectionRuleHeader.cpp
        ui/widgets/ComboBoxWheelFilter.cpp
        ui/widgets/ExoCheckBox.cpp
        models/OutputSettingsModel.cpp
        models/OutputPathPolicy.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(advanced_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(advanced_page_tests PROPERTIES AUTOMOC ON)

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
    LIBRARIES
        Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
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
        ui/widgets/CountdownSelect.cpp
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
target_link_libraries(recording_preset_store_tests PRIVATE ole32.lib shell32.lib)

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
# TransportDock, AudioSourceToggle, CountdownSelect included so tests 11-13
# (dock button seams/visibility/click) can exercise the live widget without
# requiring the full RecordPage.
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
        ui/widgets/CountdownSelect.cpp
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
