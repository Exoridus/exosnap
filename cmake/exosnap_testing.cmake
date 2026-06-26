include_guard(GLOBAL)

function(exosnap_add_gtest)
  set(options)
  set(one_value_args NAME TEST_PREFIX)
  set(multi_value_args SOURCES LIBRARIES)

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

  # Stage the FFmpeg + core Qt runtime DLLs next to the test binary so the
  # default (POST_BUILD) gtest discovery can launch it at build time without Qt
  # or FFmpeg on PATH — otherwise it fails to start (0xc0000135) on clean CI
  # runners. FFmpeg in particular is never on PATH (it lives under _deps/).
  # NOTE: do not switch discovery to DISCOVERY_MODE PRE_TEST — the CI presets
  # use the Visual Studio multi-config generator and ctest runs without a
  # configuration, which makes PRE_TEST include files resolve to an empty
  # tests-file name ("include could not find requested file").
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
    set(_exosnap_stage_commands "")
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
    add_custom_target(${_exosnap_stage_target} ${_exosnap_stage_commands}
      COMMENT "Staging FFmpeg + Qt runtime DLLs for tests in ${CMAKE_CURRENT_BINARY_DIR}"
      VERBATIM)
    set_target_properties(${_exosnap_stage_target} PROPERTIES FOLDER "exosnap/build-support")
  endif()
  add_dependencies(${ARG_NAME} ${_exosnap_stage_target})

  include(GoogleTest)
  # The post-build discovery launches the freshly-built binary to enumerate its
  # tests. On clean/cold CI runners the first launch pays for loading the staged
  # Qt + FFmpeg DLLs, which can exceed CTest's 5 s DISCOVERY_TIMEOUT default and
  # produce a spurious "process terminated due to timeout" failure (observed as a
  # transient, re-run-green flake during the 0.6.0 wave). Raise the ceiling well
  # above any real cold-start cost; it only bounds enumeration, not test runtime.
  set(_exosnap_discovery_timeout 60)
  if(ARG_TEST_PREFIX)
    gtest_discover_tests(${ARG_NAME}
      TEST_PREFIX "${ARG_TEST_PREFIX}"
      DISCOVERY_TIMEOUT ${_exosnap_discovery_timeout})
  else()
    gtest_discover_tests(${ARG_NAME}
      DISCOVERY_TIMEOUT ${_exosnap_discovery_timeout})
  endif()
endfunction()
