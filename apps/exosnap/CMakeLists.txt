cmake_minimum_required(VERSION 3.27)

# Local Qt install hint — override via Qt6_DIR or CMAKE_PREFIX_PATH.
if(NOT Qt6_DIR)
    list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.9.0/msvc2022_64")
endif()

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)

if(NOT Qt6_FOUND)
    add_custom_target(exosnap
        COMMAND ${CMAKE_COMMAND} -E echo "exosnap skipped: Qt6 not found"
    )
    message(WARNING "Qt6 not found — exosnap app will not be built. "
                    "Install Qt 6 and set Qt6_DIR or CMAKE_PREFIX_PATH.")
    return()
endif()

qt_add_executable(exosnap WIN32
    main.cpp
    startup_log.h
    startup_log.cpp
    MainWindow.h
    MainWindow.cpp
    pages/RecordPage.h
    pages/RecordPage.cpp
    pages/VideoPage.h
    pages/VideoPage.cpp
    pages/AudioPage.h
    pages/AudioPage.cpp
    pages/OutputPage.h
    pages/OutputPage.cpp
    pages/HotkeysPage.h
    pages/HotkeysPage.cpp
    pages/DiagnosticsPage.h
    pages/DiagnosticsPage.cpp
    pages/LogsPage.h
    pages/LogsPage.cpp
    pages/AdvancedPage.h
    pages/AdvancedPage.cpp
    services/RecordingCoordinator.h
    services/RecordingCoordinator.cpp
    viewmodels/RecordViewModel.h
    viewmodels/RecordViewModel.cpp
)

set_target_properties(exosnap PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME "exosnap"
)

target_link_libraries(exosnap PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    recorder_core
    exosnap::capability
    exosnap::warnings
)

target_include_directories(exosnap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
