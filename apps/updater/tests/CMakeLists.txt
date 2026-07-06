# apps/updater/tests/CMakeLists.txt -- headless unit tests for the updater logic.
#
# UpdaterController and UpdaterArgs are Qt-Core value logic; these gtest binaries
# need no QApplication. Mirrors the libs/update/tests registration pattern.

exosnap_add_gtest(
    NAME    test_updater_controller
    TEST_PREFIX updater.
    SOURCES test_updater_controller.cpp
    LIBRARIES exosnap_updater_logic
)

exosnap_add_gtest(
    NAME    test_updater_args
    TEST_PREFIX updater.
    SOURCES test_updater_args.cpp
    LIBRARIES exosnap_updater_logic
)
