# tools/soak — reliability endurance ("soak") tooling.
#
# The PURE analysis surface (SoakAbortPolicy / SoakMetricsAggregator / the JSONL +
# report serializers) lives in a small host-agnostic library with no WinAPI,
# FFmpeg, or engine linkage, so its decision math is unit-tested on the headless
# CI runner. The `exosnap-soak` executable that drives a real RecorderSession and
# samples the host process is a developer tool built on demand (EXOSNAP_BUILD_SOAK).

# --- Pure analysis library (always built; CI tests it) ---
add_library(soak_metrics STATIC
    soak_metrics.cpp
)
target_include_directories(soak_metrics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(soak_metrics PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(soak_metrics PUBLIC cxx_std_20)

exosnap_add_gtest(
    NAME soak_policy_tests
    TEST_PREFIX soak.
    SOURCES tests/test_soak_policy.cpp
    LIBRARIES soak_metrics
)
