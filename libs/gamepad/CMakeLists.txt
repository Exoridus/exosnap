# libs/gamepad/CMakeLists.txt -- gamepad input through Microsoft GameInput.
#
# Qt-free. Holds the pure state model (layout mapping, normalization, clock
# conversion, reading history) and the GameInput-backed live source. Linking the
# library loads nothing: the GameInput runtime is only resolved when
# GameInputSource::Start runs.

cmake_minimum_required(VERSION 3.25)

add_library(exosnap_gamepad STATIC
    src/gamepad.cpp
    src/game_input_source.cpp
    include/gamepad/gamepad.h
)

target_include_directories(exosnap_gamepad
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    # Header-only qpc_100ns.h: the capture clock's own tick conversion, so the
    # two clocks cannot drift apart in their arithmetic.
    PRIVATE ${PROJECT_SOURCE_DIR}/libs/engine/src
)
target_compile_definitions(exosnap_gamepad PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(exosnap_gamepad
    PRIVATE GameInput::GameInput exosnap::warnings
)

add_library(exosnap::gamepad ALIAS exosnap_gamepad)

add_subdirectory(tests)
