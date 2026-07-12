# tools/soak — reliability endurance ("soak") tooling.
#
# The PURE analysis surface (SoakAbortPolicy / SoakMetricsAggregator / the JSONL +
# report serializers) lives in a small host-agnostic library with no WinAPI,
# FFmpeg, or engine linkage, so its decision math is unit-tested on the headless
# CI runner. The SoakRunner loop + the WinAPI process sampler add the host side;
# the `exosnap-soak` executable drives a real RecorderSession or the GPU-free
# synthetic twin. Everything here builds on CI (recorder_core is always a target);
# only the REAL recording PATH needs a GPU at runtime.

# --- Pure analysis library (no WinAPI / FFmpeg / engine) ---
add_library(soak_metrics STATIC
    soak_metrics.cpp
)
target_include_directories(soak_metrics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(soak_metrics PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(soak_metrics PUBLIC cxx_std_20)

# --- Host run-loop: engine-callback latch + process sampler + timeline + abort ---
add_library(soak_runner STATIC
    soak_runner.cpp
    soak_process_sampler.cpp
)
target_include_directories(soak_runner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(soak_runner PUBLIC soak_metrics recorder_core)
target_link_libraries(soak_runner PRIVATE exosnap::warnings)
target_compile_definitions(soak_runner PRIVATE NOMINMAX)
target_compile_features(soak_runner PUBLIC cxx_std_20)

# --- exosnap-soak executable (real GPU path + synthetic twin + clapper) ---
add_executable(exosnap-soak
    main.cpp
)
target_link_libraries(exosnap-soak PRIVATE
    soak_runner
    recorder_core_testutil
    exosnap::warnings
)
target_compile_definitions(exosnap-soak PRIVATE NOMINMAX)
set_property(TARGET exosnap-soak PROPERTY CXX_STANDARD 20)

# --- Tests (CI, no live label) ---
exosnap_add_gtest(
    NAME soak_policy_tests
    TEST_PREFIX soak.
    SOURCES tests/test_soak_policy.cpp
    LIBRARIES soak_metrics
)

# Synthetic run-loop end-to-end: drives the GPU-free session through the real
# SoakRunner wiring, validates the output with the vendored libavformat, and trips
# the wired abort policy with an injected skew ramp.
exosnap_add_gtest(
    NAME soak_synthetic_tests
    TEST_PREFIX soak.
    SOURCES tests/test_soak_synthetic.cpp
    LIBRARIES soak_runner recorder_core_testutil
)
target_compile_definitions(soak_synthetic_tests PRIVATE NOMINMAX)
