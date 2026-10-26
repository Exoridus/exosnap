exosnap_add_gtest(
    NAME test_setup_controller
    TEST_PREFIX setup.
    SOURCES test_setup_controller.cpp
    LIBRARIES exosnap_setup_logic
)

exosnap_add_gtest(
    NAME test_setup_visual
    TEST_PREFIX setup.
    SOURCES test_setup_visual.cpp
    LIBRARIES Qt6::Gui
    TIMEOUT 180
)
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/SetupPreviewPath.h"
    CONTENT "#pragma once\n#define EXOSNAP_SETUP_PREVIEW_EXE R\"($<TARGET_FILE:exosnap_setup_preview>)\"\n"
)
target_include_directories(test_setup_visual PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
add_dependencies(test_setup_visual exosnap_setup_preview)

exosnap_add_gtest(
    NAME test_setup_window_lifecycle
    TEST_PREFIX setup.
    SOURCES test_setup_window_lifecycle.cpp
    LIBRARIES Qt6::Core
    PHASE desktop
    TIMEOUT 60
)
target_include_directories(test_setup_window_lifecycle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
add_dependencies(test_setup_window_lifecycle exosnap_setup_preview)
