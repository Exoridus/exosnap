cmake_minimum_required(VERSION 3.27)

# Canonical BASE version source of truth for the whole project. project(VERSION)
# only accepts numeric X.Y.Z, so prerelease identity cannot live here. Every
# other version form is derived from PROJECT_VERSION plus EXOSNAP_RELEASE_VERSION
# below — do not duplicate either elsewhere.
project(exosnap VERSION 0.9.0 LANGUAGES C CXX)

# Full release identity. Official release builds pass the complete release
# version (e.g. -DEXOSNAP_RELEASE_VERSION=0.9.0-rc4), derived from the validated
# git tag in CI. Developer builds leave it empty and get an honest
# "<PROJECT_VERSION>-dev" identity so they can never impersonate a release.
set(EXOSNAP_RELEASE_VERSION "" CACHE STRING
    "Full release version for official builds (e.g. 0.9.0-rc4). Empty = dev build (<PROJECT_VERSION>-dev).")
if(EXOSNAP_RELEASE_VERSION STREQUAL "")
    if(EXOSNAP_OFFICIAL_BUILD)
        message(FATAL_ERROR
            "EXOSNAP_OFFICIAL_BUILD=ON requires EXOSNAP_RELEASE_VERSION to be set "
            "(e.g. 0.9.0-rc4); an official build must declare its full release identity.")
    endif()
    set(EXOSNAP_FULL_VERSION "${PROJECT_VERSION}-dev")
else()
    if(NOT EXOSNAP_RELEASE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?$")
        message(FATAL_ERROR
            "EXOSNAP_RELEASE_VERSION '${EXOSNAP_RELEASE_VERSION}' is not a valid "
            "SemVer release version (expected X.Y.Z or X.Y.Z-<prerelease>).")
    endif()
    string(REGEX REPLACE "^([0-9]+\\.[0-9]+\\.[0-9]+).*$" "\\1"
        _exosnap_release_base "${EXOSNAP_RELEASE_VERSION}")
    if(NOT _exosnap_release_base STREQUAL PROJECT_VERSION)
        message(FATAL_ERROR
            "EXOSNAP_RELEASE_VERSION '${EXOSNAP_RELEASE_VERSION}' does not match the "
            "canonical base version '${PROJECT_VERSION}' from project(VERSION). "
            "Bump project(VERSION) or fix the release version input.")
    endif()
    if(EXOSNAP_RELEASE_VERSION STREQUAL "${PROJECT_VERSION}-dev")
        message(FATAL_ERROR
            "EXOSNAP_RELEASE_VERSION must not use the reserved '-dev' prerelease label; "
            "it is the marker for unofficial developer builds.")
    endif()
    set(EXOSNAP_FULL_VERSION "${EXOSNAP_RELEASE_VERSION}")
endif()
message(STATUS "ExoSnap release identity: ${EXOSNAP_FULL_VERSION} (base ${PROJECT_VERSION})")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Debug info is always EMBEDDED (/Z7), never a compiler-written PDB (/Zi).
#
# /Zi makes every cl.exe invocation write to one shared program database. Parallel
# builds then collide (fatal error C1041), and sccache cannot cache such an object
# at all, because the debug info lives outside it. /Z7 puts the debug info in the
# .obj, which keeps the cache working and the build parallel.
#
# The Release link still passes /DEBUG, so the LINKER produces exosnap.pdb from
# those embedded records. That is what symbolicates a crash minidump (ADR 0017
# § Symbol pipeline) — and a minidump without symbols is a list of addresses, not a
# diagnosis. The install tree never ships a .pdb (install(TARGETS) deploys RUNTIME
# only), so nothing leaks to end users.
if(MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo,Release>:Embedded>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
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

# Must come before any add_subdirectory(): it edits the global compile/link flags
# so every translation unit — third_party included — is instrumented.
include(cmake/exosnap_asan.cmake)

include(cmake/exosnap_warnings.cmake)
include(cmake/exosnap_testing.cmake)

# libebml/libmatroska ship cmake_minimum_required() floors below CMake's
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

# ADR 0033: present & tearing diagnostics (PresentMon ETW). This slice ships only the
# verifiable shell — the stub provider degrades to Unavailable and compiles regardless.
# The flag gates the in-process ETW consumer vendored in third_party/CMakeLists.txt;
# it must be declared BEFORE add_subdirectory(third_party) so the guarded FetchContent
# block there can see the value on a clean configure (before the option lands in cache).
option(EXOSNAP_WITH_PRESENTMON "Build the PresentMon ETW present-diagnostics provider" ON)

# third_party must come before static analysis so its targets are not analyzed.
add_subdirectory(third_party)

# FFmpeg lgpl-shared prebuilt, built and pinned from our own
# Exoridus/exosnap-ffmpeg-build repository (FFmpeg 8.1.1, LGPL-only, release r5).
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
# pull.ff is set here for the same reason: it is per-clone local config that
# cannot be committed, so configure is the only place that can carry it. With
# "only", a pull that would need a merge fails loudly instead of quietly
# recording one — which is how a stale local main goes unnoticed.
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
    execute_process(
        COMMAND ${GIT_EXECUTABLE} config pull.ff only
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        RESULT_VARIABLE _exosnap_git_pull_ff_result
        ERROR_QUIET
    )
    if(NOT _exosnap_git_pull_ff_result EQUAL 0)
        message(STATUS "pull.ff was not updated during configure; continuing.")
    endif()
endif()

# Qt's deployment API is set up ONCE, by whichever directory first calls
# find_package(Qt6), and the set of deploy helpers it makes available is read
# off the Qt6::Core target in that directory's scope. Imported targets are
# directory-scoped by default, so a sibling directory that later asks for the
# Qml component registers Qt6QmlDeploySupport.cmake on a DIFFERENT Qt6::Core and
# the generated QtDeploySupport-<CONFIG>.cmake never learns about it — the
# install step then dies on "qt6_deploy_qml_imports: Unknown CMake command".
# That is exactly what libs/control (the first find_package(Qt6) in the tree,
# Core-only) did to app/'s Qt Quick deployment. Promoting imported targets to
# global scope makes all of them the same Qt6::Core regardless of which
# subdirectory finds Qt first, so the ordering below stays a readability
# decision rather than a packaging contract.
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)

# The local Qt hint belongs HERE, not in app/, for the same reason the paragraph
# above exists: libs/control is the first directory to call find_package(Qt6), and
# it runs two add_subdirectory() lines before app/. A hint that arrives later is a
# hint that arrives after the decision it was meant to inform -- libs/control then
# takes its "Qt6 not found" branch, exosnap::control is never defined, and the
# configure dies further down on an alias nobody declared. It stayed invisible for
# as long as every tree had Qt6_DIR in its cache from an earlier configure, and on
# CI, where the Qt action exports the path into the environment; the first genuinely
# clean local configure is what surfaced it.
#
# Overridable as before: an explicit Qt6_DIR or CMAKE_PREFIX_PATH wins, because this
# only fills in when nothing else has.
if(NOT Qt6_DIR)
    list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64")
endif()

add_subdirectory(libs/engine)
add_subdirectory(libs/capability)
add_subdirectory(libs/crash_capture)
add_subdirectory(libs/update)
# Before app/ and apps/updater/: both link the shared control-channel layer and
# the shared update-handoff document (one schema, two processes).
add_subdirectory(libs/control)
add_subdirectory(libs/update_handoff)
add_subdirectory(app)
add_subdirectory(apps/updater)
add_subdirectory(tests)

# crashpad_handler.exe staging lives in app/CMakeLists.txt, where the exosnap
# target is defined (correct POST_BUILD scope) and the crashpad_handler target
# is reachable via $<TARGET_FILE:crashpad_handler>.

option(EXOSNAP_BUILD_PROBES "Build ExoSnap developer probe executables" OFF)
if(EXOSNAP_BUILD_PROBES)
    add_subdirectory(tools/probes)
endif()

# Reliability soak tooling. The subdirectory always builds the pure analysis
# library + its CI test; the GPU-driving `exosnap-soak` executable is gated
# inside on EXOSNAP_BUILD_SOAK (developer machine only).
add_subdirectory(tools/soak)

# TEST-ONLY Windows environment orchestrator (display/audio state for release
# verification). Unconditional like tools/soak so the PURE transaction library
# and its tests build on the headless CI runner; the WinAPI provider and the
# `exosnap-envctl` host are guarded by if(WIN32) inside. Nothing here is linked
# into exosnap.exe or exosnap-updater.exe, and nothing here is installed.
add_subdirectory(tools/envctl)
