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

  include(GoogleTest)
  if(ARG_TEST_PREFIX)
    gtest_discover_tests(${ARG_NAME} TEST_PREFIX "${ARG_TEST_PREFIX}")
  else()
    gtest_discover_tests(${ARG_NAME})
  endif()
endfunction()
