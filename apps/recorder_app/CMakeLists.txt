cmake_minimum_required(VERSION 3.27)

# Local Qt install hint — override via Qt6_DIR or CMAKE_PREFIX_PATH.
if(NOT Qt6_DIR)
    list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.9.0/msvc2022_64")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(recorder_app WIN32
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

set_target_properties(recorder_app PROPERTIES AUTOMOC ON)

target_link_libraries(recorder_app PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    recorder_core
    exosnap::capability
    exosnap::warnings
)

target_include_directories(recorder_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
