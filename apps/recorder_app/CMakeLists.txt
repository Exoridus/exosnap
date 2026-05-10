cmake_minimum_required(VERSION 3.27)

set(APP_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(APP_VCPROJ "${APP_PROJECT_DIR}/recorder_app.vcxproj")

find_program(MSBUILD_EXE NAMES MSBuild.exe msbuild
    PATHS
        "C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin"
        "C:/Program Files/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin"
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin"
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin"
    DOC "Path to MSBuild.exe"
)

set(APP_SOURCES
    "${APP_PROJECT_DIR}/Package.appxmanifest"
    "${APP_PROJECT_DIR}/App.xaml"
    "${APP_PROJECT_DIR}/App.xaml.h"
    "${APP_PROJECT_DIR}/App.xaml.cpp"
    "${APP_PROJECT_DIR}/MainWindow.xaml"
    "${APP_PROJECT_DIR}/MainWindow.xaml.h"
    "${APP_PROJECT_DIR}/MainWindow.xaml.cpp"
    "${APP_PROJECT_DIR}/pages/AdvancedPage.xaml"
    "${APP_PROJECT_DIR}/pages/AdvancedPage.h"
    "${APP_PROJECT_DIR}/pages/AdvancedPage.cpp"
    "${APP_PROJECT_DIR}/pages/AudioPage.xaml"
    "${APP_PROJECT_DIR}/pages/AudioPage.h"
    "${APP_PROJECT_DIR}/pages/AudioPage.cpp"
    "${APP_PROJECT_DIR}/pages/DiagnosticsPage.xaml"
    "${APP_PROJECT_DIR}/pages/DiagnosticsPage.h"
    "${APP_PROJECT_DIR}/pages/DiagnosticsPage.cpp"
    "${APP_PROJECT_DIR}/pages/HotkeysPage.xaml"
    "${APP_PROJECT_DIR}/pages/HotkeysPage.h"
    "${APP_PROJECT_DIR}/pages/HotkeysPage.cpp"
    "${APP_PROJECT_DIR}/pages/LogsPage.xaml"
    "${APP_PROJECT_DIR}/pages/LogsPage.h"
    "${APP_PROJECT_DIR}/pages/LogsPage.cpp"
    "${APP_PROJECT_DIR}/pages/OutputPage.xaml"
    "${APP_PROJECT_DIR}/pages/OutputPage.h"
    "${APP_PROJECT_DIR}/pages/OutputPage.cpp"
    "${APP_PROJECT_DIR}/pages/RecordPage.xaml"
    "${APP_PROJECT_DIR}/pages/RecordPage.h"
    "${APP_PROJECT_DIR}/pages/RecordPage.cpp"
    "${APP_PROJECT_DIR}/pages/VideoPage.xaml"
    "${APP_PROJECT_DIR}/pages/VideoPage.h"
    "${APP_PROJECT_DIR}/pages/VideoPage.cpp"
)

set(WINUI_TOOLING_AVAILABLE FALSE)

if(MSBUILD_EXE)
    find_program(VSWHERE_EXE NAMES vswhere.exe
        PATHS
            "C:/Program Files (x86)/Microsoft Visual Studio/Installer"
        NO_DEFAULT_PATH
    )
    if(VSWHERE_EXE)
        execute_process(
            COMMAND "${VSWHERE_EXE}" -latest -property installationPath
            OUTPUT_VARIABLE VS_INSTALL_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(VS_INSTALL_PATH)
            foreach(VS_VER 18.0 17.0 16.0)
                set(XAML_TARGETS "${VS_INSTALL_PATH}/MSBuild/Microsoft/WindowsXaml/v${VS_VER}/Microsoft.Windows.UI.Xaml.Cpp.targets")
                if(EXISTS "${XAML_TARGETS}")
                    set(WINUI_TOOLING_AVAILABLE TRUE)
                    break()
                endif()
            endforeach()
        endif()
    endif()
endif()

if(WINUI_TOOLING_AVAILABLE)
    add_custom_target(recorder_app ALL
        COMMAND "${MSBUILD_EXE}" "${APP_VCPROJ}"
            "/p:Configuration=$<CONFIG>"
            "/p:Platform=x64"
            "/p:OutDir=${CMAKE_BINARY_DIR}/apps/recorder_app/$<CONFIG>"
            "/p:IntDir=${CMAKE_BINARY_DIR}/apps/recorder_app/$<CONFIG>/obj"
            /nologo
            /restore
        VERBATIM
        COMMENT "Building WinUI 3 app (recorder_app)"
        SOURCES ${APP_SOURCES}
    )
    message(STATUS "WinUI 3 app (recorder_app) — included in default build (XAML targets found)")
elseif(MSBUILD_EXE)
    add_custom_target(recorder_app
        COMMAND "${MSBUILD_EXE}" "${APP_VCPROJ}"
            "/p:Configuration=$<CONFIG>"
            "/p:Platform=x64"
            "/p:OutDir=${CMAKE_BINARY_DIR}/apps/recorder_app/$<CONFIG>"
            "/p:IntDir=${CMAKE_BINARY_DIR}/apps/recorder_app/$<CONFIG>/obj"
            /nologo
            /restore
        VERBATIM
        COMMENT "Building WinUI 3 app (recorder_app)"
        SOURCES ${APP_SOURCES}
    )
    message(STATUS "WinUI 3 app (recorder_app) — NOT in default build (XAML targets missing)")
    message(WARNING "Windows App SDK C++ workload not found. recorder_app must be built explicitly:")
    message(WARNING "  cmake --build --preset windows-x64-debug --target recorder_app")
    message(WARNING "Install the component: Microsoft.VisualStudio.ComponentGroup.WindowsAppSDK.Cpp")
else()
    add_custom_target(recorder_app
        COMMAND "${CMAKE_COMMAND}" -E echo "recorder_app skipped: MSBuild.exe not found"
    )
    message(WARNING "MSBuild.exe not found. recorder_app cannot build.")
endif()
