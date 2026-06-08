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

# ---- Build metadata ----
set(EXOSNAP_VERSION "1.0-dev")

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
    ui/chrome/OperationalTitleBar.h
    ui/chrome/OperationalTitleBar.cpp
    ui/chrome/GlobalRecordingBar.h
    ui/chrome/GlobalRecordingBar.cpp
    ui/dialogs/AboutOverlay.h
    ui/dialogs/AboutOverlay.cpp
    ui/dialogs/SourcePickerPanel.h
    ui/dialogs/SourcePickerPanel.cpp
    ui/dialogs/SourcePickerDialog.h
    ui/dialogs/SourcePickerDialog.cpp
    ui/dialogs/SourcePickerOverlay.h
    ui/dialogs/SourcePickerOverlay.cpp
    ui/dialogs/SourcePickerWindowRules.h
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
    target_sources(exosnap PRIVATE exosnap.rc)
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
    recorder_core exosnap::capability exosnap::warnings
    d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib
    mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib
)

target_include_directories(exosnap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

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
        viewmodels/RecordViewModel.cpp
        diagnostics/error_message.cpp
        models/FilenameBuilder.cpp
        models/RecordingPreset.cpp
        models/OutputPathPolicy.cpp
        models/OutputSettingsModel.cpp
        diagnostics/AppLog.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core Qt6::Gui Qt6::Widgets
)
target_include_directories(completed_result_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(completed_result_tests PRIVATE ole32.lib shell32.lib)
set_target_properties(completed_result_tests PROPERTIES AUTOMOC ON)

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
        viewmodels/RecordViewModel.cpp
        diagnostics/error_message.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
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
        services/WebcamService.cpp
        models/OutputSettingsModel.cpp
        models/RecordingPreset.cpp
        models/FilenameBuilder.cpp
        models/OutputPathValidator.cpp
        models/OutputPathPolicy.cpp
        diagnostics/AppLog.cpp
    LIBRARIES Qt6::Core Qt6::Gui exosnap_capability recorder_core
)
target_include_directories(output_settings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(output_settings_tests PRIVATE mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)
set_target_properties(output_settings_tests PROPERTIES AUTOMOC ON)

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
        diagnostics/DiagnosticResult.cpp
        diagnostics/CapabilitySummary.cpp
        diagnostics/ConfigSummary.cpp
        diagnostics/RecommendationEngine.cpp
        diagnostics/SelfTestRunner.cpp
        models/OutputSettingsModel.cpp
        models/OutputPathPolicy.cpp
    LIBRARIES exosnap_capability recorder_core
)
target_include_directories(diagnostics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
        pages/ConfigPage.cpp
        viewmodels/PresentationStateBuilder.cpp
        ui/widgets/ComboBoxWheelFilter.cpp
        ui/widgets/CameraPreview.cpp
        ui/widgets/ExoToggle.cpp
        ui/widgets/VUMeterWidget.cpp
        ui/widgets/WebcamSetupPanel.cpp
        models/FilenameBuilder.cpp
        models/OutputPathPolicy.cpp
        models/OutputPathValidator.cpp
        models/RecordingPreset.cpp
        services/WebcamService.cpp
        models/OutputSettingsModel.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(config_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(config_page_tests PROPERTIES AUTOMOC ON)
target_link_libraries(config_page_tests PRIVATE mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)

exosnap_add_gtest(
    NAME source_picker_dialog_tests
    TEST_PREFIX source.
    SOURCES
        tests/test_source_picker_dialog.cpp
        ui/dialogs/SourcePickerPanel.cpp
        ui/dialogs/SourcePickerDialog.cpp
        ui/widgets/RegionGeometry.cpp
        ui/widgets/CaptureTargetCard.cpp
        ui/widgets/RegionPresetCard.cpp
        services/ThumbnailCapture.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets d3d11.lib dxgi.lib runtimeobject.lib ole32.lib
)
target_include_directories(source_picker_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(source_picker_dialog_tests PRIVATE NOMINMAX)
set_target_properties(source_picker_dialog_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME source_picker_overlay_tests
    TEST_PREFIX source.
    SOURCES
        tests/test_source_picker_overlay.cpp
        ui/dialogs/SourcePickerOverlay.cpp
        ui/dialogs/SourcePickerPanel.cpp
        ui/dialogs/SourcePickerDialog.cpp
        ui/widgets/RegionGeometry.cpp
        ui/widgets/CaptureTargetCard.cpp
        ui/widgets/RegionPresetCard.cpp
        services/ThumbnailCapture.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets d3d11.lib dxgi.lib runtimeobject.lib ole32.lib
)
target_include_directories(source_picker_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(source_picker_overlay_tests PRIVATE NOMINMAX)
set_target_properties(source_picker_overlay_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME source_picker_refresh_tests
    TEST_PREFIX source.
    SOURCES
        tests/test_source_picker_refresh.cpp
        ui/dialogs/SourcePickerPanel.cpp
        ui/dialogs/SourcePickerDialog.cpp
        ui/dialogs/SourcePickerOverlay.cpp
        ui/widgets/RegionGeometry.cpp
        ui/widgets/CaptureTargetCard.cpp
        ui/widgets/RegionPresetCard.cpp
        services/ThumbnailCapture.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets d3d11.lib dxgi.lib runtimeobject.lib ole32.lib
)
target_include_directories(source_picker_refresh_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(source_picker_refresh_tests PRIVATE NOMINMAX)
set_target_properties(source_picker_refresh_tests PROPERTIES AUTOMOC ON)

exosnap_add_gtest(
    NAME webcam_page_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_page.cpp
        pages/WebcamPage.cpp
        ui/widgets/CameraPreview.cpp
        ui/widgets/ExoToggle.cpp
        ui/widgets/ComboBoxWheelFilter.cpp
        services/WebcamService.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(webcam_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(webcam_page_tests PROPERTIES AUTOMOC ON)
target_link_libraries(webcam_page_tests PRIVATE mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)

exosnap_add_gtest(
    NAME hotkeys_page_tests
    TEST_PREFIX hotkey.
    SOURCES
        tests/test_hotkeys_page.cpp
        pages/HotkeysPage.cpp
        ui/widgets/SectionRuleHeader.cpp
        ui/widgets/KeycapChip.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
)
target_include_directories(hotkeys_page_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(hotkeys_page_tests PROPERTIES AUTOMOC ON)

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
        diagnostics/DiagnosticResult.cpp
        diagnostics/CapabilitySummary.cpp
        diagnostics/ConfigSummary.cpp
        diagnostics/RecommendationEngine.cpp
        diagnostics/SelfTestRunner.cpp
        models/OutputSettingsModel.cpp
        models/OutputPathPolicy.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets exosnap_capability recorder_core
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

exosnap_add_gtest(
    NAME audio_device_notifier_tests
    TEST_PREFIX device.
    SOURCES
        tests/test_audio_device_notifier.cpp
        services/AudioDeviceNotifier.cpp
        diagnostics/AppLog.cpp
    LIBRARIES Qt6::Core exosnap_capability recorder_core)
target_include_directories(audio_device_notifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_device_notifier_tests PRIVATE ole32.lib)
set_target_properties(audio_device_notifier_tests PROPERTIES AUTOMOC ON)
