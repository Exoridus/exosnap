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

  include(GoogleTest)
  if(ARG_TEST_PREFIX)
    gtest_discover_tests(${ARG_NAME} TEST_PREFIX "${ARG_TEST_PREFIX}")
  else()
    gtest_discover_tests(${ARG_NAME})
  endif()
endfunction()
