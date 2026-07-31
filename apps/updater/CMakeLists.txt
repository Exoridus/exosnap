# apps/updater/CMakeLists.txt -- standalone exosnap-updater executable.
#
# Ships the swap-updater process: a pure step state machine (UpdaterController)
# plus a command-line parser (UpdaterArgs), both Qt-Core-only so they are unit-
# tested headless, and a thin WIN32 GUI shell (main.cpp) that owns QApplication.

# Reuse the Qt discovered by app/CMakeLists.txt when it ran; find it here too so
# this subdirectory is order-independent.
if(NOT Qt6_FOUND)
    find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
endif()

if(NOT Qt6_FOUND)
    message(WARNING "Qt6 not found — exosnap-updater will not be built.")
    return()
endif()

# ---- Build metadata (mirrors app/CMakeLists.txt) ----
# Single source of truth: root project(... VERSION ...) + EXOSNAP_FULL_VERSION
# (release identity resolved in the root CMakeLists.txt). The ProductVersion
# string carries the full release version; numeric fields stay base-only.
set(EXOSNAP_VERSION "${EXOSNAP_FULL_VERSION}")

set(_updater_ver_tweak "${PROJECT_VERSION_TWEAK}")
if(_updater_ver_tweak STREQUAL "")
    set(_updater_ver_tweak 0)
endif()
set(EXOSNAP_VERSION_COMMA
    "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},${_updater_ver_tweak}")
set(EXOSNAP_VERSION_FILE_STR
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${_updater_ver_tweak}")

# ---- Pure logic library (Qt Core only; no widgets) ----
# Shared by the executable and the headless unit tests so the state machine and
# arg parser are compiled exactly once.
add_library(exosnap_updater_logic STATIC
    UpdaterArgs.cpp
    UpdaterArgs.h
    UpdaterController.cpp
    UpdaterController.h
    UpdaterWorker.cpp
    UpdaterWorker.h
    WindowPlacement.cpp
    WindowPlacement.h
)
# UpdaterWorker is a QObject (signals across the worker-thread boundary).
set_target_properties(exosnap_updater_logic PROPERTIES AUTOMOC ON)
target_include_directories(exosnap_updater_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(exosnap_updater_logic PUBLIC NOMINMAX)
target_link_libraries(exosnap_updater_logic
    PUBLIC Qt6::Core exosnap::update
    PRIVATE exosnap::warnings
)

# ---- Widget UI library (QtWidgets) ----
# The updater window and its two custom widgets. Split into a static library so
# the GUI executable and the widget smoke test share one AUTOMOC/AUTORCC build
# (the fonts qrc references the app's bundled ttf faces by relative path — the
# same binaries, never copied). Kept separate from exosnap_updater_logic so the
# headless controller/arg tests stay Qt-Core-only.
add_library(exosnap_updater_ui STATIC
    ProgressRing.cpp
    ProgressRing.h
    StepListWidget.cpp
    StepListWidget.h
    UpdaterWindow.cpp
    UpdaterWindow.h
    UpdaterTheme.h
    assets/updater_fonts.qrc
)
set_target_properties(exosnap_updater_ui PROPERTIES AUTOMOC ON AUTORCC ON)
target_include_directories(exosnap_updater_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(exosnap_updater_ui PUBLIC NOMINMAX)
target_link_libraries(exosnap_updater_ui
    PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets exosnap_updater_logic
    PRIVATE exosnap::warnings
)

# ---- GUI executable ----
qt_add_executable(exosnap_updater WIN32
    main.cpp
)
set_target_properties(exosnap_updater PROPERTIES AUTOMOC ON OUTPUT_NAME "exosnap-updater")
target_compile_definitions(exosnap_updater PRIVATE NOMINMAX)
target_link_libraries(exosnap_updater PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets
    exosnap_updater_logic
    exosnap_updater_ui
    exosnap::warnings
)

if(WIN32)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/updater.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/updater.rc"
        @ONLY
    )
    target_sources(exosnap_updater PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/updater.rc")
endif()

# The updater is a shipped component of the app package, not an optional extra:
# it installs next to exosnap.exe and the ZIP/MSI derive from that install tree.
# Couple it to the app target so every build entry point that produces a
# packageable exosnap.exe — the release build preset (targets: exosnap), CI, and
# `cmake --build --target exosnap` — also builds exosnap-updater.exe, keeping the
# install tree complete (the install rule below hard-fails if the exe is absent).
if(TARGET exosnap)
    add_dependencies(exosnap exosnap_updater)
endif()

# ---- Install: ship exosnap-updater.exe flat next to exosnap.exe ----
# The swap-updater is a real shipped runtime component: the app stages it (plus
# the shared Qt runtime) into a temp dir and launches it to perform the download/
# verify/close/swap/verify/relaunch flow. Without this rule the exe never lands in
# the CMake install tree, so neither the portable ZIP nor the MSI (both derived
# from that tree) contain it, and the update feature is non-functional in packaged
# builds. It installs to the prefix root exactly like exosnap.exe; the MSI
# auto-harvest and the ZIP both pick it up automatically from the staging tree.
# No separate Qt deploy is needed — windeployqt (driven by the exosnap target)
# already stages Qt6Core/Gui/Widgets and plugins/platforms flat in the same tree,
# which is exactly the runtime the updater links.
if(WIN32)
    install(TARGETS exosnap_updater RUNTIME DESTINATION .)
endif()

# ---- Dev convenience: stage the Qt runtime next to the build-tree exe ----
# The build-tree exosnap-updater.exe links Qt DLLs that live in Qt's bin dir, not
# next to the exe. windeployqt copies the Qt runtime + platform plugins so the
# build-tree binary launches without Qt on PATH (build-tree only).
if(WIN32)
    get_target_property(_qt6_core_loc Qt6::Core IMPORTED_LOCATION)
    if(NOT _qt6_core_loc)
        get_target_property(_qt6_core_loc Qt6::Core LOCATION)
    endif()
    if(_qt6_core_loc)
        get_filename_component(_qt6_bin_dir "${_qt6_core_loc}" DIRECTORY)
    endif()
    find_program(EXOSNAP_UPDATER_WINDEPLOYQT windeployqt HINTS "${_qt6_bin_dir}")
    if(EXOSNAP_UPDATER_WINDEPLOYQT)
        add_custom_command(TARGET exosnap_updater POST_BUILD
            COMMAND "${EXOSNAP_UPDATER_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                "$<IF:$<CONFIG:Debug>,--debug,--release>" "$<TARGET_FILE:exosnap_updater>"
            COMMENT "Staging Qt runtime next to exosnap-updater.exe (build tree)"
            VERBATIM
        )
    endif()
endif()

add_subdirectory(tests)
