# libs/gamepad/tests/CMakeLists.txt

exosnap_add_gtest(
    NAME        gamepad_tests
    TEST_PREFIX gamepad.
    SOURCES     test_gamepad.cpp
    LIBRARIES   exosnap::gamepad
)
