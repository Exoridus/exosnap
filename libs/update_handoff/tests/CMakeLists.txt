# libs/update_handoff/tests/CMakeLists.txt

exosnap_add_gtest(
    NAME    update_handoff_tests
    SOURCES test_update_handoff.cpp
    LIBRARIES exosnap::update_handoff
)
