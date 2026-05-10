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

add_subdirectory(third_party)
add_subdirectory(libs/recorder_core)
add_subdirectory(libs/recorder_facade)
add_subdirectory(apps/recorder_cli)
add_subdirectory(apps/probes)
add_subdirectory(tests)
