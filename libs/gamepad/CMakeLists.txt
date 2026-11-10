# libs/gamepad/CMakeLists.txt -- gamepad input through SDL3.
#
# Qt-free. Holds the pure state model (layout mapping, normalization, clock
# conversion, reading history) and the SDL3-backed live source. The runtime is
# delay-loaded so a missing DLL can be reported as a source startup failure.

cmake_minimum_required(VERSION 3.25)

add_library(exosnap_gamepad STATIC
    src/gamepad.cpp
    src/sdl_gamepad_source.cpp
    src/sdl_gamepad_state.cpp
    include/gamepad/sdl_gamepad_state.h
    include/gamepad/gamepad.h
)

target_include_directories(exosnap_gamepad
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    # Share the public, header-only capture clock contract without linking the engine.
    PRIVATE ${PROJECT_SOURCE_DIR}/libs/engine/include/exosnap/engine
)
target_compile_definitions(exosnap_gamepad PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(exosnap_gamepad
    PRIVATE SDL3::SDL3 exosnap::warnings bcrypt
    PUBLIC delayimp
)
target_link_options(exosnap_gamepad INTERFACE /DELAYLOAD:SDL3.dll)

add_library(exosnap::gamepad ALIAS exosnap_gamepad)

add_subdirectory(tests)
