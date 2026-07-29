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

# Pure planning pieces behind the worker (staged-root resolution, retry entry
# routing, msiexec command line) plus a no-network structural smoke of the
# worker's signal contract. Qt Core only; no QApplication.
exosnap_add_gtest(
    NAME    test_updater_worker_plan
    TEST_PREFIX updater.
    SOURCES test_updater_worker_plan.cpp
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

# Deterministic QWidget renders of the terminal failure matrix at representative
# Windows DPR values. Evidence is written under .workspace/live-verify/.
exosnap_add_gtest(
    NAME    test_updater_visual_proof
    TEST_PREFIX updater.
    SOURCES test_updater_visual_proof.cpp
    LIBRARIES exosnap_updater_ui
)
