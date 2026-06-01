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
    ui/theme/exosnap_theme.qrc
    ui/brand/BrandMarkWidget.h
    ui/brand/BrandMarkWidget.cpp
    ui/chrome/OperationalTitleBar.h
    ui/chrome/OperationalTitleBar.cpp
    ui/chrome/GlobalRecordingBar.h
    ui/chrome/GlobalRecordingBar.cpp
    ui/dialogs/SourcePickerDialog.h
    ui/dialogs/SourcePickerDialog.cpp
    ui/dialogs/SourcePickerWindowRules.h
    ui/widgets/SectionRuleHeader.h
    ui/widgets/SectionRuleHeader.cpp
    ui/widgets/StatusPill.h
    ui/widgets/StatusPill.cpp
    ui/widgets/PreviewSurface.h
    ui/widgets/PreviewSurface.cpp
    ui/widgets/CameraPreview.h
    ui/widgets/CameraPreview.cpp
    ui/widgets/CaptureTargetCard.h
    ui/widgets/CaptureTargetCard.cpp
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
    ui/widgets/AudioSourceRow.h
    ui/widgets/AudioSourceRow.cpp
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
    models/WebcamSettings.h
    models/OutputSettingsModel.h
    models/OutputSettingsModel.cpp
    models/RecordingProfile.h
    models/RecordingProfile.cpp
    models/RecordingProfileRegistry.h
    models/RecordingProfileRegistry.cpp
    models/VideoSettingsModel.h
    models/FilenameBuilder.h
    models/FilenameBuilder.cpp
    models/OutputPathValidator.h
    models/OutputPathValidator.cpp
    models/OutputPathPolicy.h
    models/OutputPathPolicy.cpp
    settings/AppSettingsStore.h
    settings/AppSettingsStore.cpp
    settings/ProfileExchange.h
    settings/ProfileExchange.cpp
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
)

if(WIN32)
    target_sources(exosnap PRIVATE exosnap.rc)
endif()

set_target_properties(exosnap PROPERTIES AUTOMOC ON AUTORCC ON OUTPUT_NAME "exosnap")
target_compile_definitions(exosnap PRIVATE NOMINMAX)

target_link_libraries(exosnap PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
    recorder_core exosnap::capability exosnap::warnings
    d3d11.lib dxgi.lib d3dcompiler.lib runtimeobject.lib ole32.lib
    mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib
)

target_include_directories(exosnap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

exosnap_add_gtest(
    NAME record_viewmodel_audio_tests
    TEST_PREFIX viewmodel.
    SOURCES
        tests/test_record_view_model_audio.cpp
        viewmodels/RecordViewModel.cpp
        diagnostics/error_message.cpp
    LIBRARIES exosnap_capability recorder_core
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
    NAME error_message_tests
    TEST_PREFIX diagnostics.
    SOURCES
        tests/test_error_message.cpp
        diagnostics/error_message.cpp
    LIBRARIES exosnap_capability recorder_core
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
        models/RecordingProfile.cpp
        models/RecordingProfileRegistry.cpp
        models/FilenameBuilder.cpp
        models/OutputPathValidator.cpp
        models/OutputPathPolicy.cpp
        diagnostics/AppLog.cpp
    LIBRARIES Qt6::Core Qt6::Gui exosnap_capability recorder_core
)
target_include_directories(output_settings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(output_settings_tests PRIVATE mfplat.lib mf.lib mfuuid.lib mfreadwrite.lib)

exosnap_add_gtest(
    NAME app_settings_store_tests
    TEST_PREFIX settings.
    SOURCES
        tests/test_app_settings_store.cpp
        tests/test_profile_exchange.cpp
        settings/AppSettingsStore.cpp
        settings/ProfileExchange.cpp
        models/OutputSettingsModel.cpp
        models/RecordingProfile.cpp
        models/RecordingProfileRegistry.cpp
        models/OutputPathPolicy.cpp
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

exosnap_add_gtest(
    NAME webcam_settings_tests
    TEST_PREFIX webcam.
    SOURCES
        tests/test_webcam_settings.cpp
        settings/AppSettingsStore.cpp
        models/OutputSettingsModel.cpp
        models/RecordingProfile.cpp
        models/RecordingProfileRegistry.cpp
        models/OutputPathPolicy.cpp
    LIBRARIES exosnap_capability recorder_core Qt6::Core
)
target_include_directories(webcam_settings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
    NAME config_page_tests
    TEST_PREFIX config.
    SOURCES
        tests/test_config_page.cpp
        pages/ConfigPage.cpp
        ui/widgets/ComboBoxWheelFilter.cpp
        models/FilenameBuilder.cpp
        models/OutputPathPolicy.cpp
        models/OutputPathValidator.cpp
        models/RecordingProfile.cpp
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
        ui/dialogs/SourcePickerDialog.cpp
        ui/widgets/CaptureTargetCard.cpp
        services/ThumbnailCapture.cpp
    LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets d3d11.lib dxgi.lib runtimeobject.lib ole32.lib
)
target_include_directories(source_picker_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(source_picker_dialog_tests PRIVATE NOMINMAX)
set_target_properties(source_picker_dialog_tests PROPERTIES AUTOMOC ON)

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
    NAME advanced_page_tests
    TEST_PREFIX advanced.
    SOURCES
        tests/test_advanced_page.cpp
        pages/AdvancedPage.cpp
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
