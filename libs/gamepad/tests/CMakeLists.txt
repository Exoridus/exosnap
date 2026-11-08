# libs/gamepad/tests/CMakeLists.txt

exosnap_add_gtest(
    NAME        gamepad_tests
    TEST_PREFIX gamepad.
    SOURCES     test_gamepad.cpp test_sdl_gamepad.cpp
    LIBRARIES   exosnap::gamepad SDL3::SDL3
)
add_custom_command(TARGET gamepad_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:gamepad_tests>"
    VERBATIM
)
