include_guard(GLOBAL)

function(exosnap_add_gtest)
  set(options)
  set(one_value_args NAME TEST_PREFIX TIMEOUT)
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
  string(MAKE_C_IDENTIFIER "${CMAKE_CURRENT_BINARY_DIR}" _exosnap_dir_key)
  set(_exosnap_stage_target "exosnap_stage_runtime_dlls_${_exosnap_dir_key}")
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
    foreach(_qt_target IN ITEMS Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
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
  # entry name, e.g. TEST_PREFIX "recorder_core." + NAME "test_mp4_remuxer"
  # -> CTest entry "recorder_core.test_mp4_remuxer".
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

  # Optional CTest labels. `live` marks binaries that issue real hardware queries
  # (DXGI adapter enumeration, NVENC/WASAPI probes) and therefore behave
  # differently — or only GTEST_SKIP — on GPU-/device-less runners. `ctest -LE
  # live` then runs the fully-deterministic subset with no hardware present.
  if(ARG_LABELS)
    set_tests_properties("${_exosnap_test_name}" PROPERTIES LABELS "${ARG_LABELS}")
  endif()
endfunction()
