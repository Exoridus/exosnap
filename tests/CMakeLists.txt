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
# EXOSNAP_INSTALL_TREE may be set at configure time (CI) to point at the
# cmake --install output. The test also honors a runtime EXOSNAP_INSTALL_TREE
# environment variable, and skips when neither resolves to a directory.
if(EXOSNAP_INSTALL_TREE)
    target_compile_definitions(install_tree_tests PRIVATE
        EXOSNAP_INSTALL_TREE="${EXOSNAP_INSTALL_TREE}")
endif()

exosnap_add_gtest(
  NAME portable_docs_tests
  TEST_PREFIX docs.
  SOURCES package/portable_docs_test.cpp
)
target_compile_definitions(portable_docs_tests PRIVATE
    EXOSNAP_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    EXOSNAP_PROJECT_VERSION="${PROJECT_VERSION}")
