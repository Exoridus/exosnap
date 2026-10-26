if(NOT WIN32 OR NOT MSVC)
    return()
endif()

find_package(Qt6 6.12 QUIET COMPONENTS Core Gui Qml Quick QuickControls2)
if(NOT Qt6_FOUND)
    return()
endif()
qt_standard_project_setup(REQUIRES 6.8)
qt_policy(SET QTP0001 NEW)

qt_add_library(exosnap_setup_logic STATIC SetupController.cpp SetupController.h SetupBridge.h)
target_include_directories(exosnap_setup_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(exosnap_setup_logic PUBLIC Qt6::Core PRIVATE exosnap::warnings)

qt_add_library(exosnap_setup_ui SHARED SetupMain.cpp
    ${PROJECT_SOURCE_DIR}/app/assets/fonts/exosnap_fonts.qrc)
set_target_properties(exosnap_setup_ui PROPERTIES
    OUTPUT_NAME "exosnap-setup-ui"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
    AUTORCC ON)
target_link_libraries(exosnap_setup_ui PRIVATE exosnap_setup_logic
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 exosnap::warnings shell32)
target_include_directories(exosnap_setup_ui PRIVATE "${PROJECT_SOURCE_DIR}/app"
    "${PROJECT_SOURCE_DIR}/app/quick/ExoSnap/Quick")
target_compile_definitions(exosnap_setup_ui PRIVATE NOMINMAX
    EXOSNAP_VERSION="${EXOSNAP_FULL_VERSION}")
target_compile_options(exosnap_setup_ui PRIVATE /wd4702)

set(_setup_quick_dir "${PROJECT_SOURCE_DIR}/app/quick/ExoSnap/Quick")
set(_setup_shared_qml
    "${_setup_quick_dir}/ExoTheme.qml"
    "${_setup_quick_dir}/ExoButton.qml"
    "${_setup_quick_dir}/ExoCheckBox.qml"
    "${_setup_quick_dir}/ExoGlyph.qml"
    "${_setup_quick_dir}/ExoSetupHeader.qml"
    "${_setup_quick_dir}/ExoSetupProgress.qml")
foreach(_qml IN LISTS _setup_shared_qml)
    get_filename_component(_name "${_qml}" NAME)
    set_source_files_properties("${_qml}" PROPERTIES QT_RESOURCE_ALIAS "${_name}")
endforeach()
set_source_files_properties("${_setup_quick_dir}/ExoTheme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(exosnap_setup_ui
    URI ExoSnap.Setup
    VERSION 1.0
    NO_PLUGIN
    DEPENDENCIES QtQuick QtQuick.Controls QtQuick.Layouts QtQuick.Shapes
    SOURCES SetupQmlTypes.h
        ../../app/quick/ExoSnap/Quick/QuickThemeTokens.cpp
        ../../app/quick/ExoSnap/Quick/QuickThemeTokens.h
        ../../app/quick/ExoSnap/Quick/QuickWheelMetrics.h
    QML_FILES quick/SetupMain.qml quick/SetupDecision.qml quick/SetupStatus.qml ${_setup_shared_qml})
qt_add_resources(exosnap_setup_ui setup_license PREFIX "/setup"
    BASE "${PROJECT_SOURCE_DIR}" FILES "${PROJECT_SOURCE_DIR}/LICENSE")

qt_add_executable(exosnap_setup_preview WIN32 PreviewMain.cpp)
set_target_properties(exosnap_setup_preview PROPERTIES
    OUTPUT_NAME "exosnap-setup-preview"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
target_link_libraries(exosnap_setup_preview PRIVATE exosnap_setup_ui)

include(FetchContent)
FetchContent_Declare(wix_balutil
    URL https://api.nuget.org/v3-flatcontainer/wixtoolset.balutil/4.0.5/wixtoolset.balutil.4.0.5.nupkg
    URL_HASH SHA256=3F8B1B64CEAF3569597BDFCDD643E14C56505678888DDC1B0A1AA8AB65445A3A)
FetchContent_Declare(wix_bootstrappercore
    URL https://api.nuget.org/v3-flatcontainer/wixtoolset.bootstrappercore.native/4.0.5/wixtoolset.bootstrappercore.native.4.0.5.nupkg
    URL_HASH SHA256=773CA413E155C703B6C335E1EB54ECC258A0A2EDE4A07739910A05A64347BC5B)
FetchContent_Declare(wix_dutil
    URL https://api.nuget.org/v3-flatcontainer/wixtoolset.dutil/4.0.5/wixtoolset.dutil.4.0.5.nupkg
    URL_HASH SHA256=2A07D064491B03121DAC9BA35723556EC73A83871043092678801EA97019C331)
FetchContent_MakeAvailable(wix_balutil wix_bootstrappercore wix_dutil)

add_library(exosnap_setup_ba SHARED native/Bootstrapper.cpp)
set_target_properties(exosnap_setup_ba PROPERTIES
    OUTPUT_NAME "exosnap-setup-ba"
    MSVC_RUNTIME_LIBRARY MultiThreaded
    AUTOMOC OFF AUTOUIC OFF)
target_compile_definitions(exosnap_setup_ba PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# The SDK ships release static libraries. Keep the shim's CRT consistent in Debug too.
target_compile_definitions(exosnap_setup_ba PRIVATE _ITERATOR_DEBUG_LEVEL=0)
target_include_directories(exosnap_setup_ba SYSTEM PRIVATE
    "${wix_balutil_SOURCE_DIR}/build/native/include"
    "${wix_bootstrappercore_SOURCE_DIR}/build/native/include"
    "${wix_dutil_SOURCE_DIR}/build/native/include")
target_link_libraries(exosnap_setup_ba PRIVATE
    "${wix_balutil_SOURCE_DIR}/build/native/v14/x64/balutil.lib"
    "${wix_dutil_SOURCE_DIR}/build/native/v14/x64/dutil.lib"
    ole32 oleaut32 shell32 shlwapi version msi advapi32 wininet)
target_compile_options(exosnap_setup_ba PRIVATE /W4 /WX /EHsc /utf-8)

get_target_property(_setup_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
get_filename_component(_setup_qt_bin "${_setup_qt_core}" DIRECTORY)
get_filename_component(_setup_qt_root "${_setup_qt_bin}" DIRECTORY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/SetupPreviewPlugins.h"
    CONTENT "#pragma once\n#define EXOSNAP_SETUP_PREVIEW_PLUGINS R\"(${_setup_qt_root}/plugins/platforms)\"\n")
target_include_directories(exosnap_setup_preview PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
find_program(EXOSNAP_SETUP_WINDEPLOYQT windeployqt HINTS "${_setup_qt_bin}" REQUIRED NO_DEFAULT_PATH)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_DEBUG_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)
if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    message(FATAL_ERROR "Setup requires app-local MSVC runtime files before Burn installs the system runtime")
endif()
set(_setup_compiler_runtime)
foreach(_runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    if(_runtime MATCHES "/Debug_NonRedist/")
        list(APPEND _setup_compiler_runtime "$<$<CONFIG:Debug>:${_runtime}>")
    else()
        list(APPEND _setup_compiler_runtime "${_runtime}")
    endif()
endforeach()
add_custom_command(TARGET exosnap_setup_ui POST_BUILD
    COMMAND "${EXOSNAP_SETUP_WINDEPLOYQT}" --force --dir "$<TARGET_FILE_DIR:exosnap_setup_ui>"
        --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/quick"
        --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime
        --skip-plugin-types qmltooling "$<IF:$<CONFIG:Debug>,--debug,--release>"
        "$<TARGET_FILE:exosnap_setup_ui>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_setup_compiler_runtime}
        "$<TARGET_FILE_DIR:exosnap_setup_ui>"
    COMMAND_EXPAND_LISTS
    VERBATIM)
if(TARGET exosnap)
    add_dependencies(exosnap exosnap_setup_ba exosnap_setup_ui)
endif()
add_subdirectory(tests)
