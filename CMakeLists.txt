cmake_minimum_required(VERSION 3.27)
project(exosnap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

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
    )
endif()

add_subdirectory(libs/recorder_core)
add_subdirectory(libs/capability)
add_subdirectory(libs/recorder_facade)
add_subdirectory(apps/recorder_cli)
add_subdirectory(apps/exosnap)
add_subdirectory(apps/probes)
add_subdirectory(tests)
