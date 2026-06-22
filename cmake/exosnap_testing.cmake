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

  # Copy the FFmpeg DLLs next to the test binary BEFORE registering test
  # discovery: POST_BUILD commands run in registration order, and the default
  # (POST_BUILD) discovery launches the binary at build time. Targets that pull
  # FFmpeg-dependent objects out of recorder_core would otherwise fail to start
  # (0xc0000135) during discovery on clean CI runners.
  # NOTE: do not switch discovery to DISCOVERY_MODE PRE_TEST — the CI presets
  # use the Visual Studio multi-config generator and ctest runs without a
  # configuration, which makes PRE_TEST include files resolve to an empty
  # tests-file name ("include could not find requested file").
  foreach(_ffmpeg_dll IN LISTS EXOSNAP_FFMPEG_DLLS)
    add_custom_command(TARGET ${ARG_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_ffmpeg_dll}"
          "$<TARGET_FILE_DIR:${ARG_NAME}>"
      VERBATIM
    )
  endforeach()

  # Stage core Qt DLLs next to the test binary so gtest discovery works without
  # Qt on PATH on clean CI runners.  GUI tests in app/<config>/ already get full
  # Qt deployment from the exosnap POST_BUILD; this covers the unit-test binaries
  # that live in test-specific sub-directories.
  foreach(_qt_target IN ITEMS Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
    if(TARGET ${_qt_target})
      add_custom_command(TARGET ${ARG_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${_qt_target}>"
            "$<TARGET_FILE_DIR:${ARG_NAME}>"
        VERBATIM
      )
    endif()
  endforeach()

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
