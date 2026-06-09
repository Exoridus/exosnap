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

exosnap_add_gtest(
  NAME install_tree_tests
  TEST_PREFIX install.
  SOURCES package/install_tree_test.cpp
)
# EXOSNAP_INSTALL_TREE is set by CI to point at the cmake --install output.
# When unset, the tests probe well-known locations and skip if none exist.
if(EXOSNAP_INSTALL_TREE)
    target_compile_definitions(install_tree_tests PRIVATE
        EXOSNAP_INSTALL_TREE="${EXOSNAP_INSTALL_TREE}")
endif()
