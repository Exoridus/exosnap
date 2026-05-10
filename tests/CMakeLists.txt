exosnap_add_gtest(
  NAME recorder_smoke_tests
  SOURCES smoke/smoke_test.cpp
  LIBRARIES recorder_core
)

exosnap_add_gtest(
  NAME recorder_logging_tests
  SOURCES logging/logging_test.cpp
  LIBRARIES recorder_core nlohmann_json::nlohmann_json
)
