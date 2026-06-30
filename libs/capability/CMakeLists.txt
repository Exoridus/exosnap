add_library(exosnap_capability STATIC
  src/config_types.cpp
  src/container_compat_registry.cpp
  src/capability_set.cpp
  src/codec_selection.cpp
  src/capability_builder.cpp
  src/runtime_query.cpp
  src/resolver.cpp
  src/option_query.cpp
  src/translation.cpp
  src/audio_ui_state.cpp
)

add_library(exosnap::capability ALIAS exosnap_capability)

target_include_directories(exosnap_capability PUBLIC include)

# NVENC vendor header (third_party/nvidia/nvEncodeAPI.h) enables the per-GPU codec-GUID
# probe in runtime_query.cpp. The probe is __has_include-guarded, so a tree without the
# header still builds (degrades to nvenc_codec_probed=false).
target_include_directories(exosnap_capability PRIVATE "${CMAKE_SOURCE_DIR}/third_party/nvidia")

target_link_libraries(exosnap_capability
  PUBLIC
    recorder_core
  PRIVATE
    exosnap::warnings
    mf
    mfplat
    mfuuid
    dxgi
    d3d11
    ole32
)

exosnap_add_gtest(
  NAME container_compat_registry_tests
  TEST_PREFIX capability.
  SOURCES tests/test_container_compat_registry.cpp
  LIBRARIES exosnap_capability
)

exosnap_add_gtest(
  NAME capability_codec_selection_tests
  TEST_PREFIX capability.
  SOURCES tests/test_codec_selection.cpp
  LIBRARIES exosnap_capability
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
