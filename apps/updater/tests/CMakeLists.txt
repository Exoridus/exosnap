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

# Widget smoke test: UpdaterWindow / ProgressRing / StepListWidget are QtWidgets,
# so this binary owns its own QApplication (CTest isolates each binary in its own
# process). It links the UI library, which already carries the AUTOMOC output.
exosnap_add_gtest(
    NAME    test_updater_window_smoke
    TEST_PREFIX updater.
    SOURCES test_updater_window_smoke.cpp
    LIBRARIES exosnap_updater_ui
)
