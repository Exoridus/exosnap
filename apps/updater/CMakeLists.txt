# apps/updater/CMakeLists.txt -- standalone exosnap-updater executable.
#
# Ships the swap-updater process: a pure step state machine (UpdaterController)
# plus a command-line parser (UpdaterArgs), both Qt-Core-only so they are unit-
# tested headless, and a Qt Quick frontend (ExoSnap.Updater) that owns the
# window. No Qt Widgets: the updater process is QGuiApplication + QML.

# Reuse the Qt discovered by app/CMakeLists.txt when it ran; find it here too so
# this subdirectory is order-independent.
if(NOT Qt6_FOUND OR NOT TARGET Qt6::Qml)
    # The SAME floor app/ requires. Without it this probe accepts any Qt6 it finds
    # -- including one a stale cache still points at -- caches that decision, and
    # then app/'s 6.12 probe fails against it. One version statement per tree, or
    # the first directory to ask silently decides for the last one.
    find_package(Qt6 6.12 QUIET COMPONENTS Core Gui Qml Quick QuickControls2)
endif()

# QTP0001 puts a QML module's resources under /qt/qml, which is one of the
# engine's default import paths. Without it the module lands at :/ExoSnap/...,
# which nothing looks at, and loadFromModule comes back with no root object.
# app/ gets the same policy from qt_standard_project_setup(), whose scope does
# not reach this sibling directory.
qt_policy(SET QTP0001 NEW)

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
    UpdaterAutomation.cpp
    UpdaterAutomation.h
    UpdaterCommandPolicy.cpp
    UpdaterCommandPolicy.h
    UpdaterController.cpp
    UpdaterController.h
    UpdaterControlDispatcher.cpp
    UpdaterControlDispatcher.h
    UpdaterControlSource.h
    UpdaterExitCode.cpp
    UpdaterExitCode.h
    UpdaterFaultInjection.cpp
    UpdaterFaultInjection.h
    UpdaterWorker.cpp
    UpdaterWorker.h
    WindowPlacement.cpp
    WindowPlacement.h
)
# UpdaterWorker is a QObject (signals across the worker-thread boundary).
set_target_properties(exosnap_updater_logic PROPERTIES AUTOMOC ON)
target_include_directories(exosnap_updater_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(exosnap_updater_logic PUBLIC NOMINMAX)
# exosnap::control: the shared control-channel layer (protocol, policy
# mechanics, session, named-pipe transport). The updater's automation endpoint is
# the same channel the application exposes, from the same code -- see
# UpdaterControlDispatcher.h. Inert without --automation-control.
target_link_libraries(exosnap_updater_logic
    PUBLIC Qt6::Core exosnap::update exosnap::control exosnap::update_handoff
    PRIVATE exosnap::warnings
)

# ---- Quick UI: the QML module lives on the executable ----
# The window is rendered by Qt Quick from the ExoSnap.Updater module. The module
# is declared on the executable (the same shape the main application uses) so its
# resources and typed registration travel in the binary; there is no second
# frontend to keep in sync.
#
# The shared design sources come from the application's own Quick directory by
# path -- one canonical file each, never a copy. Their names inside this module
# are the same basenames, which is why the aliases below are set explicitly:
# QT_RESOURCE_ALIAS is what makes a shared file addressable from a module whose
# source directory is not its own.
set(_updater_quick_dir "${PROJECT_SOURCE_DIR}/app/quick/ExoSnap/Quick")
set(_updater_shared_qml
    "${_updater_quick_dir}/ExoTheme.qml"
    "${_updater_quick_dir}/ExoButton.qml"
    "${_updater_quick_dir}/ExoGlyph.qml"
)
foreach(_shared_qml IN LISTS _updater_shared_qml)
    get_filename_component(_shared_name "${_shared_qml}" NAME)
    set_source_files_properties("${_shared_qml}" PROPERTIES QT_RESOURCE_ALIAS "${_shared_name}")
endforeach()
# The theme is a singleton here exactly as it is in the application module; the
# pragma in the file is not read from outside the module's own source directory.
set_source_files_properties("${_updater_quick_dir}/ExoTheme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# ---- GUI executable ----
qt_add_executable(exosnap_updater WIN32
    main.cpp
    UpdaterViewAdapter.cpp
    UpdaterViewAdapter.h
    ${PROJECT_SOURCE_DIR}/app/assets/fonts/exosnap_fonts.qrc
)

set_target_properties(exosnap_updater PROPERTIES AUTOMOC ON AUTORCC ON OUTPUT_NAME "exosnap-updater")
target_compile_definitions(exosnap_updater PRIVATE NOMINMAX)
# QuickThemeTokens.cpp reads the shared theme table, which is header-only and
# lives with the application's theme code.
target_include_directories(exosnap_updater PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/app"
    "${_updater_quick_dir}"
    "${PROJECT_SOURCE_DIR}/libs/capability/include"
)
target_link_libraries(exosnap_updater PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    exosnap_updater_logic
    exosnap::warnings
)
# Same as every QML test runner in the tree: qmlcachegen pulls in Qt QML headers
# that trip C4702 under this project's /WX.
target_compile_options(exosnap_updater PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/wd4702>
)

qt_add_qml_module(exosnap_updater
    URI ExoSnap.Updater
    VERSION 1.0
    DEPENDENCIES
        QtQuick
        QtQuick.Controls
        QtQuick.Layouts
        # ExoGlyph and UpdaterRing draw with Shape and PathSvg.
        QtQuick.Shapes
    SOURCES
        ../../app/quick/ExoSnap/Quick/QuickThemeTokens.cpp
        ../../app/quick/ExoSnap/Quick/QuickThemeTokens.h
        ../../app/quick/ExoSnap/Quick/QuickWheelMetrics.h
    QML_FILES
        quick/UpdaterMain.qml
        quick/UpdaterGlyph.qml
        quick/UpdaterRing.qml
        quick/UpdaterStepList.qml
        ${_updater_shared_qml}
)

if(WIN32)
    # The application's own brand icon, compiled into the updater as its first
    # icon resource. Windows picks the LOWEST-numbered icon resource for the
    # taskbar, Explorer and the window, so without this the updater shows the
    # generic executable icon in the taskbar while the user is watching it
    # replace ExoSnap. Forward slashes on purpose: the resource compiler accepts
    # them and a configured Windows path would otherwise embed escape sequences.
    set(EXOSNAP_UPDATER_ICON "${PROJECT_SOURCE_DIR}/app/assets/brand/exosnap-app.ico")
    if(NOT EXISTS "${EXOSNAP_UPDATER_ICON}")
        message(FATAL_ERROR "Updater icon not found: ${EXOSNAP_UPDATER_ICON}")
    endif()
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/updater.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/updater.rc"
        @ONLY
    )
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/updater.rc"
        APPEND PROPERTY OBJECT_DEPENDS "${EXOSNAP_UPDATER_ICON}")
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

# The launch-plan test stages the shipped updater from its own file list, so it
# needs the exe built even though the dependency direction (app/ configured
# first) means it cannot be declared there.
if(TARGET update_launch_plan_tests)
    add_dependencies(update_launch_plan_tests exosnap_updater)
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
# already stages the full Quick runtime and its `qml/` import trees flat in the
# same tree, which is what the updater module imports at runtime.
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
    # find_program skips a cached hit, and this cache outlives a Qt upgrade: the
    # tree was upgraded to 6.11.2 while the entry still named 6.11.1's
    # windeployqt, which staged a mixed runtime next to the updater and made it
    # refuse to start ("Cannot mix incompatible Qt library").
    if(EXOSNAP_UPDATER_WINDEPLOYQT)
        string(FIND "${EXOSNAP_UPDATER_WINDEPLOYQT}" "${_qt6_bin_dir}" _windeploy_matches_qt)
        if(NOT _windeploy_matches_qt EQUAL 0)
            unset(EXOSNAP_UPDATER_WINDEPLOYQT CACHE)
            find_program(EXOSNAP_UPDATER_WINDEPLOYQT windeployqt HINTS "${_qt6_bin_dir}")
        endif()
    endif()
    if(EXOSNAP_UPDATER_WINDEPLOYQT)
        add_custom_command(TARGET exosnap_updater POST_BUILD
            COMMAND "${EXOSNAP_UPDATER_WINDEPLOYQT}" --force --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/quick"
                --no-translations --no-system-d3d-compiler --no-opengl-sw
                --no-compiler-runtime --skip-plugin-types qmltooling
                "$<IF:$<CONFIG:Debug>,--debug,--release>" "$<TARGET_FILE:exosnap_updater>"
            COMMENT "Staging Qt runtime next to exosnap-updater.exe (build tree)"
            VERBATIM
        )
    endif()
    # The offscreen platform plugin for the BUILD TREE only: windeployqt stages
    # the windows plugin the product runs on, while the updater's headless smoke
    # and evidence tests render the real window offscreen. It is deliberately not
    # part of UpdaterStagingFileList().
    get_filename_component(_qt6_plugins_dir "${_qt6_bin_dir}/../plugins/platforms" ABSOLUTE)
    add_custom_command(TARGET exosnap_updater POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${_qt6_plugins_dir}/qoffscreend.dll,${_qt6_plugins_dir}/qoffscreen.dll>"
            "$<TARGET_FILE_DIR:exosnap_updater>/platforms/"
        COMMENT "Staging the offscreen platform plugin for the updater tests (build tree)"
        VERBATIM
    )
endif()

add_subdirectory(tests)
