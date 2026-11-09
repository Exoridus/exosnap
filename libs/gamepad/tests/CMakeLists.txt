# libs/gamepad/tests/CMakeLists.txt

exosnap_add_gtest(
    NAME        gamepad_tests
    TEST_PREFIX gamepad.
    SOURCES     test_gamepad.cpp
    LIBRARIES   exosnap::gamepad
)

# Compile the shared clock without access to private engine headers.
add_library(gamepad_clock_header_check OBJECT test_public_clock_header.cpp)
target_compile_features(gamepad_clock_header_check PRIVATE cxx_std_20)
target_include_directories(gamepad_clock_header_check PRIVATE
    ${PROJECT_SOURCE_DIR}/libs/engine/include/exosnap/engine)
target_link_libraries(gamepad_clock_header_check PRIVATE exosnap::warnings)
