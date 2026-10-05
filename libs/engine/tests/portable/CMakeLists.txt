cmake_minimum_required(VERSION 3.27)
project(exosnap_concurrency LANGUAGES CXX)

include(FetchContent)
include(CTest)
include(../../../../cmake/exosnap_asan.cmake)

option(EXOSNAP_TSAN "Instrument the portable production synchronization tests with ThreadSanitizer" OFF)

if(EXOSNAP_TSAN)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "EXOSNAP_TSAN requires Clang on Linux. It does not cover Windows workers or drivers.")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571
)
set(BUILD_GMOCK OFF CACHE INTERNAL "")
set(INSTALL_GTEST OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(googletest)

add_executable(exosnap_concurrency_tests
    ../test_mux_queue.cpp
    ../test_session_synchronization.cpp
    ../test_session_callback_gate.cpp
)
target_compile_features(exosnap_concurrency_tests PRIVATE cxx_std_20)
set_target_properties(exosnap_concurrency_tests PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(exosnap_concurrency_tests PRIVATE ../../src ../../include)
target_link_libraries(exosnap_concurrency_tests PRIVATE GTest::gtest_main)
exosnap_stage_asan_runtime(exosnap_concurrency_tests)
if(MSVC)
    target_compile_options(exosnap_concurrency_tests PRIVATE /W4 /WX /w14062)
else()
    target_compile_options(exosnap_concurrency_tests PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME engine.portable_concurrency COMMAND exosnap_concurrency_tests)
set_tests_properties(engine.portable_concurrency PROPERTIES TIMEOUT 120 LABELS phase.cpu)
