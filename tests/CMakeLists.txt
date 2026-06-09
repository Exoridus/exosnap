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

exosnap_add_gtest(
  NAME license_bundle_tests
  TEST_PREFIX license.
  SOURCES package/license_bundle_test.cpp
)
target_compile_definitions(license_bundle_tests PRIVATE
    EXOSNAP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
