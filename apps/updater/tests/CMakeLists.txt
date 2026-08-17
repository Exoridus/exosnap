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

# The process exit contract. The pure half is a table over every phase; the
# process half launches the SHIPPED exe for the two codes a real invocation can
# produce before any window exists, so main()'s early returns are covered too.
# EXOSNAP_UPDATER_EXE is a build-time generator expression, never a path in the
# source tree.
exosnap_add_gtest(
    NAME    test_updater_exit_codes
    TEST_PREFIX updater.
    SOURCES test_updater_exit_codes.cpp
    LIBRARIES exosnap_updater_logic
)
# The path goes through a generated header with a RAW string literal, not
# through a /D define: a Windows path in a normal string literal turns "\Users"
# into an invalid escape sequence and fails to compile.
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/UpdaterExePath.h"
    CONTENT "#pragma once\n#define EXOSNAP_UPDATER_EXE R\"($<TARGET_FILE:exosnap_updater>)\"\n"
)
target_include_directories(test_updater_exit_codes
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
add_dependencies(test_updater_exit_codes exosnap_updater)

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

# The automation endpoint's precondition table and published state, and the
# session/settle/revision contract on top of it. Both are pure: no pipe, no
# window, no worker -- which is what lets the whole refusal surface be covered
# instead of sampled by driving a real installation into each state.
exosnap_add_gtest(
    NAME    test_updater_command_policy
    TEST_PREFIX updater.
    SOURCES test_updater_command_policy.cpp
    LIBRARIES exosnap_updater_logic
)

exosnap_add_gtest(
    NAME    test_updater_control_dispatcher
    TEST_PREFIX updater.
    SOURCES test_updater_control_dispatcher.cpp
    LIBRARIES exosnap_updater_logic
)

# Pure geometry behind placing the updater window near the ExoSnap window
# it was launched for (centered, clamped inside the target monitor's work
# area). Qt Core only (QRect/QPoint/QSize); no QApplication needed.
exosnap_add_gtest(
    NAME    test_window_placement
    TEST_PREFIX updater.
    SOURCES test_window_placement.cpp
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

# Layout, state-presentation and accessibility contracts measured on the real
# widget tree rather than on pixels: long-version elision, the long msiexec
# detail, the indeterminate pre-flight ring, the eyebrow that names the run, the
# version emphasis after a terminal failure, and the accessible progress/step
# semantics. Writes no evidence files.
exosnap_add_gtest(
    NAME    test_updater_text_layout
    TEST_PREFIX updater.
    SOURCES test_updater_text_layout.cpp
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
