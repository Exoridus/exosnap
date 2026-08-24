# tools/soak — reliability endurance ("soak") tooling.
#
# The PURE analysis surface (SoakAbortPolicy / SoakMetricsAggregator / the JSONL +
# report serializers) lives in a small host-agnostic library with no WinAPI,
# FFmpeg, or engine linkage, so its decision math is unit-tested on the headless
# CI runner. The SoakRunner loop + the WinAPI process sampler add the host side;
# the `exosnap-soak` executable drives a real RecorderSession or the GPU-free
# synthetic twin. Everything here builds on CI (exosnap_engine is always a target);
# only the REAL recording PATH needs a GPU at runtime.

# --- Pure analysis library (no WinAPI / FFmpeg / engine) ---
add_library(soak_metrics STATIC
    soak_metrics.cpp
)
target_include_directories(soak_metrics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(soak_metrics PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(soak_metrics PUBLIC cxx_std_20)

# --- Pure clapper CLI/schedule validation (no WinAPI / engine) ---
add_library(clapper_schedule STATIC
    clapper_schedule.cpp
)
target_include_directories(clapper_schedule PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(clapper_schedule PUBLIC cxx_std_20)

# --- Host run-loop: engine-callback latch + process sampler + timeline + abort ---
add_library(soak_runner STATIC
    soak_runner.cpp
    soak_process_sampler.cpp
)
target_include_directories(soak_runner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(soak_runner PUBLIC soak_metrics exosnap_engine)
target_link_libraries(soak_runner PRIVATE exosnap::warnings)
target_compile_definitions(soak_runner PRIVATE NOMINMAX)
target_compile_features(soak_runner PUBLIC cxx_std_20)

# --- exosnap-soak executable (real GPU path + synthetic twin + clapper) ---
add_executable(exosnap-soak
    main.cpp
)
target_link_libraries(exosnap-soak PRIVATE
    clapper_schedule
    soak_runner
    exosnap_engine_testutil
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

exosnap_add_gtest(
    NAME clapper_schedule_tests
    TEST_PREFIX soak.
    SOURCES tests/test_clapper_schedule.cpp
    LIBRARIES clapper_schedule
)

# Synthetic run-loop end-to-end: drives the GPU-free session through the real
# SoakRunner wiring, validates the output with the vendored libavformat, and trips
# the wired abort policy with an injected skew ramp.
exosnap_add_gtest(
    NAME soak_synthetic_tests
    TEST_PREFIX soak.
    SOURCES tests/test_soak_synthetic.cpp
    LIBRARIES soak_runner exosnap_engine_testutil
)
target_compile_definitions(soak_synthetic_tests PRIVATE NOMINMAX)

if(Python3_EXECUTABLE)
    add_test(
        NAME soak.av_sync_analyzer_unit
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/dev/test_av_sync_check.py"
    )
    set_tests_properties(soak.av_sync_analyzer_unit PROPERTIES TIMEOUT 30)
endif()
