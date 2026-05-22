add_library(exosnap_capability STATIC
  src/config_types.cpp
  src/capability_set.cpp
  src/capability_builder.cpp
  src/runtime_query.cpp
  src/resolver.cpp
  src/option_query.cpp
  src/translation.cpp
  src/audio_ui_state.cpp
)

add_library(exosnap::capability ALIAS exosnap_capability)

target_include_directories(exosnap_capability PUBLIC include)

target_link_libraries(exosnap_capability
  PUBLIC
    recorder_core
  PRIVATE
    exosnap::warnings
    mf
    mfplat
    mfuuid
    dxgi
    ole32
)

exosnap_add_gtest(
  NAME capability_compatibility_matrix_tests
  TEST_PREFIX capability.
  SOURCES tests/test_compatibility_matrix.cpp
  LIBRARIES exosnap_capability
)

exosnap_add_gtest(
  NAME capability_resolver_tests
  TEST_PREFIX capability.
  SOURCES tests/test_resolver.cpp
  LIBRARIES exosnap_capability
)

exosnap_add_gtest(
  NAME capability_option_query_tests
  TEST_PREFIX capability.
  SOURCES tests/test_option_query.cpp
  LIBRARIES exosnap_capability
)

exosnap_add_gtest(
  NAME capability_runtime_merge_tests
  TEST_PREFIX capability.
  SOURCES tests/test_runtime_merge.cpp
  LIBRARIES exosnap_capability
)

exosnap_add_gtest(
  NAME capability_audio_plan_builder_tests
  TEST_PREFIX capability.
  SOURCES tests/test_audio_plan_builder.cpp
  LIBRARIES exosnap_capability
)

exosnap_add_gtest(
  NAME audio_ui_state_tests
  TEST_PREFIX capability.
  SOURCES tests/test_audio_ui_state.cpp
  LIBRARIES exosnap_capability
)
