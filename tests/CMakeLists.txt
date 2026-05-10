add_executable(recorder_smoke_tests
  smoke/smoke_test.cpp
)

target_link_libraries(recorder_smoke_tests PRIVATE
  GTest::gtest_main
  recorder_core
  exosnap::warnings
)

include(GoogleTest)
gtest_discover_tests(recorder_smoke_tests)
