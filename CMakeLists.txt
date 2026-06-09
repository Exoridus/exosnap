cmake_minimum_required(VERSION 3.27)
project(exosnap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

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

# third_party must come before static analysis so its targets are not analyzed.
add_subdirectory(third_party)

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
add_subdirectory(apps/recorder_cli)
add_subdirectory(apps/exosnap)
add_subdirectory(apps/probes)
add_subdirectory(tests)
