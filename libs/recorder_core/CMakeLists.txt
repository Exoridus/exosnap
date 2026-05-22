set(_nvenc_header "${CMAKE_SOURCE_DIR}/third_party/nvidia/nvEncodeAPI.h")

if(NOT EXISTS "${_nvenc_header}")
    message(STATUS "recorder_core: nvEncodeAPI.h not found at ${_nvenc_header}")
    message(STATUS "  Place nvEncodeAPI.h in third_party/nvidia/ to enable the full recorder_core build")
    message(STATUS "  Falling back to skeleton build (version + logging only)")

    add_library(recorder_core STATIC
        src/version.cpp
        src/logging/logging.cpp
        src/audio_input_device.cpp
        src/audio_track_model.cpp
        src/codec_private.cpp
        include/recorder_core/audio_meter.h
        src/audio_meter.cpp
        src/opus_audio_encoder.cpp
    )

    target_include_directories(recorder_core PUBLIC include)

    set_target_properties(recorder_core PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    target_link_libraries(recorder_core PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Opus::opus
        mfuuid.lib
        ole32.lib
        exosnap::warnings
    )

    exosnap_add_gtest(
        NAME test_audio_track_resolver
        TEST_PREFIX recorder_core.
        SOURCES tests/test_audio_track_resolver.cpp
        LIBRARIES recorder_core
    )

    exosnap_add_gtest(
        NAME test_multi_track_packet_types
        TEST_PREFIX recorder_core.
        SOURCES tests/test_multi_track_packet_types.cpp
        LIBRARIES recorder_core
    )

    exosnap_add_gtest(
        NAME test_codec_private_readiness
        TEST_PREFIX recorder_core.
        SOURCES tests/test_codec_private_readiness.cpp
        LIBRARIES recorder_core
    )
    target_include_directories(test_codec_private_readiness PRIVATE src)

    exosnap_add_gtest(
        NAME test_opus_encoder
        TEST_PREFIX recorder_core.
        SOURCES tests/test_opus_encoder.cpp
        LIBRARIES recorder_core
    )
    target_include_directories(test_opus_encoder PRIVATE src)

    exosnap_add_gtest(
        NAME test_audio_meter
        TEST_PREFIX recorder_core.
        SOURCES tests/test_audio_meter.cpp
        LIBRARIES recorder_core
    )

    return()
endif()

# ---------------------------------------------------------------------------
# Full build — all M3.1 sources
# ---------------------------------------------------------------------------

FetchContent_GetProperties(libwebm SOURCE_DIR _libwebm_src)

add_library(recorder_core STATIC
    src/version.cpp
    src/logging/logging.cpp

    src/recorder_session.cpp
    src/audio_input_device.cpp
    src/codec_private.cpp
    include/recorder_core/audio_meter.h
    src/audio_meter.cpp
    src/wgc_capture.cpp
    src/nvenc_encoder.cpp
    src/mf_aac_encoder.cpp
    src/mixed_audio_src.cpp
    src/wasapi_capture_src.cpp
    src/wasapi_loopback.cpp
    src/wasapi_loopback_src.cpp
    src/wasapi_process_loopback_src.cpp
    src/wasapi_process_loopback_src.h
    src/opus_audio_encoder.cpp
    src/session_stats_collector.cpp
    src/video_thread.cpp
    src/audio_thread.cpp
    src/mux_thread.cpp
    src/audio_track_model.cpp
)

target_include_directories(recorder_core
    PUBLIC  include
    PRIVATE src
            "${CMAKE_SOURCE_DIR}/third_party/nvidia"
            "${_libwebm_src}"
)

set_target_properties(recorder_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(recorder_core
    PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Opus::opus
        mkvmuxer
        d3d11.lib
        dxgi.lib
        dwmapi.lib
        user32.lib
        runtimeobject.lib
        ole32.lib
        mfplat.lib
        mf.lib
        mfuuid.lib
        wmcodecdspuuid.lib
        exosnap::warnings
)

exosnap_add_gtest(
    NAME test_audio_track_resolver
    TEST_PREFIX recorder_core.
    SOURCES tests/test_audio_track_resolver.cpp
    LIBRARIES recorder_core
)

exosnap_add_gtest(
    NAME test_multi_track_packet_types
    TEST_PREFIX recorder_core.
    SOURCES tests/test_multi_track_packet_types.cpp
    LIBRARIES recorder_core
)

exosnap_add_gtest(
    NAME test_codec_private_readiness
    TEST_PREFIX recorder_core.
    SOURCES tests/test_codec_private_readiness.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_codec_private_readiness PRIVATE src)

exosnap_add_gtest(
    NAME test_opus_encoder
    TEST_PREFIX recorder_core.
    SOURCES tests/test_opus_encoder.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_opus_encoder PRIVATE src)

exosnap_add_gtest(
    NAME test_audio_thread_source_agnostic
    TEST_PREFIX recorder_core.
    SOURCES tests/test_audio_thread_source_agnostic.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_audio_thread_source_agnostic PRIVATE src)

exosnap_add_gtest(
    NAME test_mic_channel_mapping
    TEST_PREFIX recorder_core.
    SOURCES tests/test_mic_channel_mapping.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_mic_channel_mapping PRIVATE src)

exosnap_add_gtest(
    NAME test_audio_plan_validate
    TEST_PREFIX recorder_core.
    SOURCES tests/test_audio_plan_validate.cpp
    LIBRARIES recorder_core
)

exosnap_add_gtest(
    NAME test_audio_source_semantics
    TEST_PREFIX recorder_core.
    SOURCES tests/test_audio_source_semantics.cpp
    LIBRARIES recorder_core
)

exosnap_add_gtest(
    NAME test_audio_duration_stats
    TEST_PREFIX recorder_core.
    SOURCES tests/test_audio_duration_stats.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_audio_duration_stats PRIVATE src)

exosnap_add_gtest(
    NAME test_audio_meter
    TEST_PREFIX recorder_core.
    SOURCES tests/test_audio_meter.cpp
    LIBRARIES recorder_core
)

exosnap_add_gtest(
    NAME mixed_audio_src_tests
    TEST_PREFIX recorder_core.
    SOURCES tests/test_mixed_audio_src.cpp
    LIBRARIES recorder_core
)
target_include_directories(mixed_audio_src_tests PRIVATE src)
