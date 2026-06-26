cmake_minimum_required(VERSION 3.27)

# Canonical version source of truth for the whole project. Every other version
# form (application metadata, Windows VERSIONINFO, About surface, release
# artifact name) is derived from PROJECT_VERSION — do not duplicate it elsewhere.
project(exosnap VERSION 0.7.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Use embedded debug info (/Z7) instead of PDB (/Zi) to prevent parallel-build
# PDB contention with the Visual Studio generator. Ninja builds are unaffected
# because CMake already adds /FS for Ninja+MSVC. /Z7 embeds debug info in each
# .obj file; debugging experience is equivalent to /Zi.
#
# EXOSNAP_RELEASE_PDB (default OFF): the release pipeline turns this ON so the
# Release config emits a SEPARATE .pdb (/Zi + linker /DEBUG) that CI can archive
# for offline symbolication of crash minidumps (ADR 0017 § Symbol pipeline). The
# default OFF build is unchanged: plain Release stays PDB-free, and the install
# tree never ships a .pdb regardless (install(TARGETS) deploys RUNTIME only).
option(EXOSNAP_RELEASE_PDB
    "Emit a separate PDB for the Release config so the release pipeline can archive symbols."
    OFF)
if(MSVC)
    if(EXOSNAP_RELEASE_PDB)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>$<$<CONFIG:Release>:ProgramDatabase>")
        # /Zi alone records debug info in the objects; the linker must be told to
        # produce the .pdb and keep it (Release otherwise strips debug data).
        add_link_options("$<$<CONFIG:Release>:/DEBUG>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
    else()
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
    endif()
endif()

option(EXOSNAP_USE_SCCACHE "Use sccache as the C++ compiler launcher when it is available." OFF)
set(EXOSNAP_COMPILER_LAUNCHER "" CACHE STRING "Optional C++ compiler launcher executable, such as sccache.")

if(EXOSNAP_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${EXOSNAP_COMPILER_LAUNCHER}")
elseif(EXOSNAP_USE_SCCACHE)
    find_program(EXOSNAP_SCCACHE_EXE NAMES sccache)
    if(EXOSNAP_SCCACHE_EXE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${EXOSNAP_SCCACHE_EXE}")
        message(STATUS "Compiler launcher: sccache (${EXOSNAP_SCCACHE_EXE})")
    else()
        message(WARNING "EXOSNAP_USE_SCCACHE is ON, but sccache was not found. Configure continues without a compiler launcher.")
    endif()
endif()

enable_testing()

include(cmake/exosnap_warnings.cmake)
include(cmake/exosnap_testing.cmake)

# libebml/libmatroska/fdk-aac ship cmake_minimum_required() floors below CMake's
# current minimum. Setting the policy version minimum lets FetchContent configure
# them without per-developer command-line flags. Applies to all presets/generators.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# These FetchContent dependencies are statically linked into exosnap and must
# not leak development files (headers, static libs, cmake config) into the
# runtime install tree.  The libraries have unconditional install() rules that
# cannot be disabled through upstream options.  This install CODE overwrites
# their cmake_install.cmake scripts before the subdirectory includes run,
# so the install graph produces the correct tree directly.
set(_exosnap_suppressed_deps
    libopus-build
    fdk-aac-build
    ebml-build
    libmatroska-build
)
install(CODE "
  foreach(_d IN ITEMS ${_exosnap_suppressed_deps})
    set(_p \"${PROJECT_BINARY_DIR}/_deps/\${_d}/cmake_install.cmake\")
    if(EXISTS \"\${_p}\")
      file(WRITE \"\${_p}\" \"# Install rules suppressed by exosnap — dependency is statically linked.\\n\")
    endif()
  endforeach()
")

# third_party must come before static analysis so its targets are not analyzed.
add_subdirectory(third_party)

# FFmpeg lgpl-shared prebuilt (BtbN pinned).
# Provides FFmpeg::avformat, FFmpeg::avcodec, FFmpeg::avutil, FFmpeg::swresample,
# and the convenience bundle FFmpeg::mux.
include(cmake/VendorFFmpeg.cmake)

# sentry-native crash capture (optional; off by default).
# Set EXOSNAP_ENABLE_CRASH_CAPTURE=ON to fetch and link sentry-native.
# Set EXOSNAP_OFFICIAL_BUILD=ON (release pipeline only) to compile in the DSN.
# See cmake/VendorSentry.cmake for license audit, spike verdict, and usage notes.
include(cmake/VendorSentry.cmake)

# Sets CMAKE_CXX_CLANG_TIDY / CMAKE_CXX_CPPCHECK for all subsequent targets.
# Only active when EXOSNAP_CLANG_TIDY / EXOSNAP_CPPCHECK are ON (lint preset).
include(cmake/exosnap_static_analysis.cmake)

# Auto-configure the git hooks path so pre-commit and pre-push are picked up
# automatically after every fresh clone, without a separate install step.
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} config core.hooksPath .githooks
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        RESULT_VARIABLE _exosnap_git_hooks_result
        ERROR_QUIET
    )
    if(NOT _exosnap_git_hooks_result EQUAL 0)
        message(STATUS "Git hooks path was not updated during configure; continuing.")
    endif()
endif()

add_subdirectory(libs/recorder_core)
add_subdirectory(libs/capability)
add_subdirectory(libs/recorder_facade)
add_subdirectory(libs/crash_capture)
add_subdirectory(libs/update)
add_subdirectory(app)
add_subdirectory(tests)

# crashpad_handler.exe staging lives in app/CMakeLists.txt, where the exosnap
# target is defined (correct POST_BUILD scope) and the crashpad_handler target
# is reachable via $<TARGET_FILE:crashpad_handler>.

option(EXOSNAP_BUILD_PROBES "Build ExoSnap developer probe executables" OFF)
if(EXOSNAP_BUILD_PROBES)
    add_subdirectory(tools/probes)
endif()
