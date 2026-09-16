include_guard(GLOBAL)

# The execution phases a test can belong to, and what each one claims about the
# machine it needs. Every CTest entry carries exactly one of them as a
# `phase.<name>` label, so a caller can ask for a phase instead of guessing from a
# test's name which runner it is safe on.
#
#   hermetic  Nothing outside the process. No hardware query, no network, no
#             desktop, no machine state. Runs anywhere, including a container.
#   cpu       Real CPU work or real time: an encode, a soak, a timing assertion.
#             Deterministic in result, not in duration, so it belongs off the
#             fastest lane rather than off a headless runner.
#   gpu       Needs a graphics adapter: a D3D11 device, DXGI enumeration, NVENC.
#             On a runner without one these can only skip, and a phase that skips
#             is not a phase that passed.
#   desktop   Needs an interactive desktop: a window, focus, a Qt GUI surface.
#   vm        Needs a disposable machine of its own.
#   human     Needs a person to do something no API can do.
#
# The order is deliberate: each phase needs everything the ones before it need.
set(EXOSNAP_TEST_PHASES hermetic cpu gpu desktop vm human)

function(exosnap_add_gtest)
  set(options)
  set(one_value_args NAME TEST_PREFIX TIMEOUT PHASE)
  set(multi_value_args SOURCES LIBRARIES LABELS)

  cmake_parse_arguments(
    ARG
    "${options}"
    "${one_value_args}"
    "${multi_value_args}"
    ${ARGN}
  )

  if(NOT ARG_NAME)
    message(FATAL_ERROR "exosnap_add_gtest: NAME is required")
  endif()

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "exosnap_add_gtest: SOURCES is required")
  endif()

  add_executable(${ARG_NAME} ${ARG_SOURCES})

  target_link_libraries(${ARG_NAME} PRIVATE
    GTest::gtest_main
    exosnap::warnings
    ${ARG_LIBRARIES}
  )

  # Stage the FFmpeg + core Qt runtime DLLs next to the test binary so it can be
  # launched at CTest time without Qt or FFmpeg on PATH — otherwise it fails to
  # start (0xc0000135) on clean CI runners. FFmpeg in particular is never on
  # PATH (it lives under _deps/). The test exe target depends on the stage
  # target below, so building the tests always refreshes the staged DLLs even
  # though we no longer launch the exe at build time (see the add_test note near
  # the end of this function — one CTest entry per binary, no discovery run).
  #
  # CONCURRENCY: every exosnap_add_gtest target in a given CMakeLists lands its
  # exe in the SAME output directory (e.g. all of libs/update/tests, or the 84
  # app/ test binaries). Giving each target its own POST_BUILD copy_if_different
  # of the same dll into that shared directory makes parallel builds race — two
  # copies open the identical destination file at once and one fails with
  # "Error copying file" (a transient, re-run-green CI flake; observed staging
  # swresample-6.dll into libs/update/tests during the 0.7.0 wave). Instead stage
  # the DLLs with a SINGLE custom target per output directory and have every test
  # target in that directory depend on it: the copies then run exactly once,
  # serially, so two writers never touch the same file.
  #
  # The key is the binary directory RELATIVE to the build root, never its
  # absolute path. It ends up inside a generated batch file name under
  # CMakeFiles/, and an absolute path pushes that file past MAX_PATH in a deep
  # build tree (a git worktree under .claude/worktrees/ reaches 269 characters):
  # ninja then cannot launch it at all -- "CreateProcess: The filename or
  # extension is too long" -- and NO target in the tree builds. Relative is just
  # as unique, because a directory appears exactly once in one build tree.
  file(RELATIVE_PATH _exosnap_dir_relative "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  if(_exosnap_dir_relative STREQUAL "")
    set(_exosnap_dir_relative "root")
  endif()
  string(MAKE_C_IDENTIFIER "${_exosnap_dir_relative}" _exosnap_dir_key)
  set(_exosnap_stage_target "exosnap_stage_runtime_dlls_${_exosnap_dir_key}")
  # MAKE_C_IDENTIFIER maps `/` and `_` to the same underscore, so two DIFFERENT
  # directories can produce one key (libs/update/tests and libs/update_handoff...
  # are one rename apart). Silently, that makes the second directory reuse the
  # first one's stage target and stage its DLLs into the wrong output folder --
  # every test binary there then fails to START with 0xC0000135, which reads as a
  # broken build rather than a name collision. Claimed keys are tracked so the
  # collision is a configure error instead.
  get_property(_exosnap_stage_owner GLOBAL PROPERTY "exosnap_stage_key_${_exosnap_dir_key}")
  if(_exosnap_stage_owner AND NOT _exosnap_stage_owner STREQUAL "${CMAKE_CURRENT_BINARY_DIR}")
    message(FATAL_ERROR
      "Runtime-DLL stage key '${_exosnap_dir_key}' is claimed by '${_exosnap_stage_owner}' and requested again "
      "by '${CMAKE_CURRENT_BINARY_DIR}'. MAKE_C_IDENTIFIER collapses '/' and '_' to the same character; rename "
      "one of the two directories so their keys differ.")
  endif()
  set_property(GLOBAL PROPERTY "exosnap_stage_key_${_exosnap_dir_key}" "${CMAKE_CURRENT_BINARY_DIR}")
  if(NOT TARGET ${_exosnap_stage_target})
    # The stage target can run before MSBuild creates the per-config output
    # directory; `cmake -E copy_if_different` into a missing directory then
    # creates a FILE with the config's name (e.g. "Debug") which blocks the
    # real directory and fails the build with MSB3191. Create it first.
    set(_exosnap_stage_commands
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${ARG_NAME}>")
    foreach(_ffmpeg_dll IN LISTS EXOSNAP_FFMPEG_DLLS)
      list(APPEND _exosnap_stage_commands
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_ffmpeg_dll}" "$<TARGET_FILE_DIR:${ARG_NAME}>")
    endforeach()
    # Empty unless EXOSNAP_ASAN=ON. The ASan runtime lives next to cl.exe and is
    # never on PATH, so an instrumented test binary fails to start (0xC0000135)
    # without it. It rides the shared per-directory stage target for the same
    # reason the FFmpeg DLLs do — see the CONCURRENCY note above.
    foreach(_asan_dll IN LISTS EXOSNAP_ASAN_RUNTIME_DLLS)
      list(APPEND _exosnap_stage_commands
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_asan_dll}" "$<TARGET_FILE_DIR:${ARG_NAME}>")
    endforeach()
    # The Quick modules are staged for the same reason as the Widgets ones, and
    # they are NOT optional: the Qt Quick frontend's tests link Qt6::Quick but
    # land in the SAME shared per-directory output as every other app test, and
    # the Quick runtime deployed for the application sits somewhere else
    # entirely. Missing here, a test binary does not fail — it fails to START
    # (0xC0000135), which on an interactive desktop is a modal "Qt6Quickd.dll not
    # found" dialog and a process that hangs until CTest gives up. Guarded by
    # if(TARGET) like the rest, so a build without Qt Declarative stages nothing.
    foreach(_qt_target IN ITEMS Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::Qml Qt6::QmlMeta Qt6::QmlModels
                                Qt6::QmlWorkerScript Qt6::Quick Qt6::QuickControls2 Qt6::QuickControls2Impl
                                Qt6::QuickLayouts Qt6::QuickTemplates2 Qt6::QuickTest Qt6::Test Qt6::OpenGL
                                Qt6::Network)
      if(TARGET ${_qt_target})
        list(APPEND _exosnap_stage_commands
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:${_qt_target}>" "$<TARGET_FILE_DIR:${ARG_NAME}>")
      endif()
    endforeach()
    # Qt resolves platform plugins relative to the loaded QtCore — the copy
    # staged next to the test exe, NOT the Qt install — so without a platforms/
    # subdirectory any QApplication test aborts at startup ("no Qt platform
    # plugin could be initialized"; interactively that is a modal dialog plus a
    # hung process). Stage the windows + offscreen platform plugins alongside.
    set(_exosnap_platforms_dir_created FALSE)
    foreach(_qt_plugin IN ITEMS Qt6::QWindowsIntegrationPlugin Qt6::QOffscreenIntegrationPlugin)
      if(TARGET ${_qt_plugin})
        if(NOT _exosnap_platforms_dir_created)
          list(APPEND _exosnap_stage_commands
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${ARG_NAME}>/platforms")
          set(_exosnap_platforms_dir_created TRUE)
        endif()
        list(APPEND _exosnap_stage_commands
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:${_qt_plugin}>" "$<TARGET_FILE_DIR:${ARG_NAME}>/platforms")
      endif()
    endforeach()
    add_custom_target(${_exosnap_stage_target} ${_exosnap_stage_commands}
      COMMENT "Staging FFmpeg + Qt runtime DLLs for tests in ${CMAKE_CURRENT_BINARY_DIR}"
      VERBATIM)
    set_target_properties(${_exosnap_stage_target} PROPERTIES FOLDER "exosnap/build-support")
  endif()
  add_dependencies(${ARG_NAME} ${_exosnap_stage_target})

  # Register ONE CTest entry per test BINARY (not per gtest case). We deliberately
  # do NOT use gtest_discover_tests here:
  #   * Discovery launched the freshly-built exe at BUILD time to enumerate its
  #     ~N gtest cases into individual CTest entries. Across ~176 binaries that
  #     produced ~2900 entries — i.e. ~2900 process spawns + QApplication inits
  #     at `ctest` time, which dominated total test wall-clock, and one extra exe
  #     launch per binary at build time.
  #   * One entry per binary keeps the runtime cost to ~176 spawns. gtest_main
  #     still runs every case inside the process and `--output-on-failure` prints
  #     the exact failing `Suite.Case` line, so failure diagnosis is unchanged.
  #   * It also sidesteps the multi-config discovery trap: the CI presets use a
  #     multi-config generator (Visual Studio locally; Ninja Multi-Config-style
  #     invocation via `ctest -C <cfg>`), where POST_TEST/PRE_TEST discovery
  #     files could resolve to an empty config name.
  #
  # `add_test(... COMMAND <target>)` expands <target> to its built exe path via a
  # generator expression, so on multi-config generators `ctest` still needs
  # `-C <config>` (already the case in every preset and CI invocation).
  #
  # TEST_PREFIX semantics are preserved by folding the prefix into the single
  # entry name, e.g. TEST_PREFIX "engine." + NAME "test_mp4_remuxer"
  # -> CTest entry "engine.test_mp4_remuxer".
  set(_exosnap_test_name "${ARG_TEST_PREFIX}${ARG_NAME}")
  add_test(NAME "${_exosnap_test_name}" COMMAND ${ARG_NAME})

  # Per-binary timeout. Default 300 s is comfortably above any current binary
  # (the slowest run a handful of seconds); override via TIMEOUT for a binary
  # that legitimately needs longer. Because the whole binary is one entry, this
  # bounds the sum of its cases, not a single case.
  if(ARG_TIMEOUT)
    set(_exosnap_timeout ${ARG_TIMEOUT})
  else()
    set(_exosnap_timeout 300)
  endif()
  set_tests_properties("${_exosnap_test_name}" PROPERTIES TIMEOUT ${_exosnap_timeout})

  # The execution phase, as a label, on every entry without exception. Default
  # `hermetic`: a gtest binary that was given no phase links against the libraries
  # and asks the machine for nothing, which is what hermetic means. A binary that
  # does need something says so, and the guard test refuses the combination that
  # would be a lie -- `live` together with `phase.hermetic`.
  if(ARG_PHASE)
    set(_exosnap_phase "${ARG_PHASE}")
  else()
    set(_exosnap_phase "hermetic")
  endif()

  if(NOT _exosnap_phase IN_LIST EXOSNAP_TEST_PHASES)
    message(FATAL_ERROR
      "exosnap_add_gtest(${ARG_NAME}): PHASE '${_exosnap_phase}' is not one of ${EXOSNAP_TEST_PHASES}")
  endif()

  # Optional CTest labels alongside it. `live` marks binaries that issue real
  # hardware queries (DXGI adapter enumeration, NVENC/WASAPI probes) and therefore
  # behave differently -- or only GTEST_SKIP -- on GPU-/device-less runners.
  # `ctest -LE live` then runs the fully-deterministic subset with no hardware.
  set(_exosnap_labels "phase.${_exosnap_phase}")
  if(ARG_LABELS)
    list(APPEND _exosnap_labels ${ARG_LABELS})
  endif()

  set_tests_properties("${_exosnap_test_name}" PROPERTIES LABELS "${_exosnap_labels}")
endfunction()

# Declares the execution phase of a test registered with a bare `add_test`.
#
# Separate from exosnap_add_gtest because a script test, a QML runner and a probe
# are all registered directly, and every CTest entry has to carry a phase for the
# guard to mean anything.
function(exosnap_set_test_phase test_name phase)
  if(NOT phase IN_LIST EXOSNAP_TEST_PHASES)
    message(FATAL_ERROR
      "exosnap_set_test_phase(${test_name}): phase '${phase}' is not one of ${EXOSNAP_TEST_PHASES}")
  endif()

  get_test_property("${test_name}" LABELS _exosnap_existing)
  if(_exosnap_existing STREQUAL "NOTFOUND")
    set(_exosnap_existing "")
  endif()

  list(APPEND _exosnap_existing "phase.${phase}")
  set_tests_properties("${test_name}" PROPERTIES LABELS "${_exosnap_existing}")
endfunction()
