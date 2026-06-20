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
        src/pcm_audio_encoder.cpp
        src/flac_audio_encoder.cpp
        src/mic_meter_service.cpp
        src/loopback_meter_service.cpp
        include/recorder_core/mp4_remuxer.h
        src/mp4_remuxer.cpp
    )

    target_include_directories(recorder_core PUBLIC include)

    set_target_properties(recorder_core PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    target_link_libraries(recorder_core PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Opus::opus
        FLAC::FLAC
        FFmpeg::mux
        mfuuid.lib
        ole32.lib
        exosnap::warnings
    )

    target_compile_definitions(recorder_core PUBLIC EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC=0)

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

    # Audio v2 (0.6.0): PCM passthrough encoder (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_pcm_audio_encoder
        TEST_PREFIX recorder_core.
        SOURCES tests/test_pcm_audio_encoder.cpp
                src/pcm_audio_encoder.cpp
    )
    target_include_directories(test_pcm_audio_encoder PRIVATE src include)

    # Audio v2 (0.6.0): FLAC lossless encoder (links libFLAC; not pure-logic).
    exosnap_add_gtest(
        NAME test_flac_audio_encoder
        TEST_PREFIX recorder_core.
        SOURCES tests/test_flac_audio_encoder.cpp
                src/flac_audio_encoder.cpp
        LIBRARIES FLAC::FLAC
    )
    target_include_directories(test_flac_audio_encoder PRIVATE src include)

    exosnap_add_gtest(
        NAME test_audio_meter
        TEST_PREFIX recorder_core.
        SOURCES tests/test_audio_meter.cpp
        LIBRARIES recorder_core
    )

    exosnap_add_gtest(
        NAME test_mic_meter_service
        TEST_PREFIX recorder_core.
        SOURCES tests/test_mic_meter_service.cpp
        LIBRARIES recorder_core
    )
    target_include_directories(test_mic_meter_service PRIVATE src)

    # Audio v2 (0.6.0): GainDbToLinear unit tests + per-row gain propagation.
    exosnap_add_gtest(
        NAME test_audio_gain_model
        TEST_PREFIX recorder_core.
        SOURCES tests/test_audio_gain_model.cpp
        LIBRARIES recorder_core
    )

    # Audio v2 (0.6.0): brickwall limiter DSP (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_brickwall_limiter
        TEST_PREFIX recorder_core.
        SOURCES tests/test_brickwall_limiter.cpp
                src/brickwall_limiter.cpp
    )
    target_include_directories(test_brickwall_limiter PRIVATE src)

    # Audio v2 (0.6.0): mic high-pass filter DSP (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_high_pass_filter
        TEST_PREFIX recorder_core.
        SOURCES tests/test_high_pass_filter.cpp
                src/high_pass_filter.cpp
    )
    target_include_directories(test_high_pass_filter PRIVATE src)

    # Audio v2 (0.6.0): mic noise gate DSP (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_noise_gate
        TEST_PREFIX recorder_core.
        SOURCES tests/test_noise_gate.cpp
                src/noise_gate.cpp
    )
    target_include_directories(test_noise_gate PRIVATE src)

    # Audio v2 (0.6.0): mic automatic gain control DSP (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_automatic_gain_control
        TEST_PREFIX recorder_core.
        SOURCES tests/test_automatic_gain_control.cpp
                src/automatic_gain_control.cpp
    )
    target_include_directories(test_automatic_gain_control PRIVATE src)

    # Audio v2 (0.6.0): MicDspAudioSrc decorator (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_mic_dsp_audio_src
        TEST_PREFIX recorder_core.
        SOURCES tests/test_mic_dsp_audio_src.cpp
                src/mic_dsp_audio_src.cpp
                src/high_pass_filter.cpp
                src/noise_gate.cpp
                src/automatic_gain_control.cpp
    )
    target_include_directories(test_mic_dsp_audio_src PRIVATE src include)

    # test_mp4_remux_pipeline_flow: pure-logic tests; no FFmpeg runtime needed.
    exosnap_add_gtest(
        NAME test_mp4_remux_pipeline_flow
        TEST_PREFIX recorder_core.
        SOURCES tests/test_mp4_remux_pipeline_flow.cpp
        LIBRARIES recorder_core
    )

    add_executable(test_mp4_remuxer tests/test_mp4_remuxer.cpp)
    target_link_libraries(test_mp4_remuxer PRIVATE
        GTest::gtest_main
        exosnap::warnings
        recorder_core EBML::ebml Matroska::matroska FFmpeg::mux
    )
    target_include_directories(test_mp4_remuxer PRIVATE src)
    # Copy FFmpeg DLLs next to the test binary so it can start.
    foreach(_ffmpeg_dll IN LISTS EXOSNAP_FFMPEG_DLLS)
        add_custom_command(TARGET test_mp4_remuxer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_ffmpeg_dll}"
                "$<TARGET_FILE_DIR:test_mp4_remuxer>"
            VERBATIM
        )
    endforeach()
    include(GoogleTest)
    # DLL copies above are registered before discovery, so the build-time
    # discovery run finds them. PRE_TEST must not be used (see exosnap_testing.cmake).
    gtest_discover_tests(test_mp4_remuxer TEST_PREFIX "recorder_core.")

    return()
endif()

# ---------------------------------------------------------------------------
# Full build — all M3.1 sources
# ---------------------------------------------------------------------------

add_library(recorder_core STATIC
    src/version.cpp
    src/logging/logging.cpp

    src/recorder_session.cpp
    src/audio_input_device.cpp
    src/codec_private.cpp
    include/recorder_core/audio_meter.h
    src/audio_meter.cpp
    src/wgc_capture.cpp
    src/dxgi_od_capture_src.cpp
    src/nvenc_encoder.cpp
    src/nvenc_video_encoder.cpp
    src/mf_aac_encoder.cpp
    src/fdk_aac_encoder.cpp
    src/gpu_compositor.cpp
    src/gpu_compositor.h
    src/mixed_audio_src.cpp
    src/brickwall_limiter.cpp
    src/brickwall_limiter.h
    src/high_pass_filter.cpp
    src/high_pass_filter.h
    src/noise_gate.cpp
    src/noise_gate.h
    src/automatic_gain_control.cpp
    src/automatic_gain_control.h
    src/mic_dsp_audio_src.cpp
    src/mic_dsp_audio_src.h
    src/wasapi_capture_src.cpp
    src/wasapi_loopback.cpp
    src/wasapi_loopback_src.cpp
    src/wasapi_process_loopback_src.cpp
    src/wasapi_process_loopback_src.h
    src/opus_audio_encoder.cpp
    src/pcm_audio_encoder.cpp
    src/flac_audio_encoder.cpp
    src/session_stats_collector.cpp
    src/pipeline_diagnostics_aggregator.cpp
    src/pipeline_diagnostics_aggregator.h
    include/recorder_core/pipeline_diagnostics.h
    src/video_thread.cpp
    src/audio_thread.cpp
    src/mux_thread.cpp
    src/matroska_stream_writer.cpp
    src/annexb_to_avcc.cpp
    src/audio_track_model.cpp
    src/mic_meter_service.cpp
    src/loopback_meter_service.cpp
    include/recorder_core/mp4_remuxer.h
    src/mp4_remuxer.cpp
)

target_include_directories(recorder_core
    PUBLIC  include
    PRIVATE src
            "${CMAKE_SOURCE_DIR}/third_party/nvidia"
)

set_target_properties(recorder_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(recorder_core
    PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Opus::opus
        FLAC::FLAC
        EBML::ebml
        Matroska::matroska
        fdk-aac
        FFmpeg::mux
        d3d11.lib
        d3dcompiler.lib
        dxgi.lib
        dwmapi.lib
        gdi32.lib
        user32.lib
        runtimeobject.lib
        ole32.lib
        mfplat.lib
        mf.lib
        mfuuid.lib
        wmcodecdspuuid.lib
        exosnap::warnings
)

target_compile_definitions(recorder_core PUBLIC EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC=1)

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

# Audio v2 (0.6.0): PCM passthrough encoder (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_pcm_audio_encoder
    TEST_PREFIX recorder_core.
    SOURCES tests/test_pcm_audio_encoder.cpp
            src/pcm_audio_encoder.cpp
)
target_include_directories(test_pcm_audio_encoder PRIVATE src include)

# Audio v2 (0.6.0): FLAC lossless encoder (links libFLAC; not pure-logic).
exosnap_add_gtest(
    NAME test_flac_audio_encoder
    TEST_PREFIX recorder_core.
    SOURCES tests/test_flac_audio_encoder.cpp
            src/flac_audio_encoder.cpp
    LIBRARIES FLAC::FLAC
)
target_include_directories(test_flac_audio_encoder PRIVATE src include)

exosnap_add_gtest(
    NAME test_audio_encoding_params
    TEST_PREFIX recorder_core.
    SOURCES tests/test_audio_encoding_params.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_audio_encoding_params PRIVATE src)

exosnap_add_gtest(
    NAME test_audio_thread_source_agnostic
    TEST_PREFIX recorder_core.
    SOURCES tests/test_audio_thread_source_agnostic.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_audio_thread_source_agnostic PRIVATE src)

exosnap_add_gtest(
    NAME test_mf_aac_encoder
    TEST_PREFIX recorder_core.
    SOURCES tests/test_mf_aac_encoder.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_mf_aac_encoder PRIVATE src)

exosnap_add_gtest(
    NAME test_fdk_aac_encoder
    TEST_PREFIX recorder_core.
    SOURCES tests/test_fdk_aac_encoder.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_fdk_aac_encoder PRIVATE src)

exosnap_add_gtest(
    NAME test_nvenc_video_encoder_interface
    TEST_PREFIX recorder_core.
    SOURCES tests/test_nvenc_video_encoder_interface.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_nvenc_video_encoder_interface PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

# test_nvenc_rc_params: pure unit tests for ComputeNvencRcParams (no GPU needed).
exosnap_add_gtest(
    NAME test_nvenc_rc_params
    TEST_PREFIX recorder_core.
    SOURCES tests/test_nvenc_rc_params.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_nvenc_rc_params PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

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
    NAME test_webm_container_validation
    TEST_PREFIX recorder_core.
    SOURCES tests/test_webm_container_validation.cpp
    LIBRARIES recorder_core
)

exosnap_add_gtest(
    NAME test_mp4_container_validation
    TEST_PREFIX recorder_core.
    SOURCES tests/test_mp4_container_validation.cpp
    LIBRARIES recorder_core
)

exosnap_add_gtest(
    NAME test_annexb_to_avcc
    TEST_PREFIX recorder_core.
    SOURCES tests/test_annexb_to_avcc.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_annexb_to_avcc PRIVATE src)

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
    NAME test_mic_meter_service
    TEST_PREFIX recorder_core.
    SOURCES tests/test_mic_meter_service.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_mic_meter_service PRIVATE src)

# Audio v2 (0.6.0): GainDbToLinear unit tests + per-row gain propagation.
exosnap_add_gtest(
    NAME test_audio_gain_model
    TEST_PREFIX recorder_core.
    SOURCES tests/test_audio_gain_model.cpp
    LIBRARIES recorder_core
)

# Audio v2 (0.6.0): brickwall limiter DSP (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_brickwall_limiter
    TEST_PREFIX recorder_core.
    SOURCES tests/test_brickwall_limiter.cpp
            src/brickwall_limiter.cpp
)
target_include_directories(test_brickwall_limiter PRIVATE src)

# Audio v2 (0.6.0): mic high-pass filter DSP (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_high_pass_filter
    TEST_PREFIX recorder_core.
    SOURCES tests/test_high_pass_filter.cpp
            src/high_pass_filter.cpp
)
target_include_directories(test_high_pass_filter PRIVATE src)

# Audio v2 (0.6.0): mic noise gate DSP (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_noise_gate
    TEST_PREFIX recorder_core.
    SOURCES tests/test_noise_gate.cpp
            src/noise_gate.cpp
)
target_include_directories(test_noise_gate PRIVATE src)

# Audio v2 (0.6.0): mic automatic gain control DSP (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_automatic_gain_control
    TEST_PREFIX recorder_core.
    SOURCES tests/test_automatic_gain_control.cpp
            src/automatic_gain_control.cpp
)
target_include_directories(test_automatic_gain_control PRIVATE src)

# Audio v2 (0.6.0): MicDspAudioSrc decorator (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_mic_dsp_audio_src
    TEST_PREFIX recorder_core.
    SOURCES tests/test_mic_dsp_audio_src.cpp
            src/mic_dsp_audio_src.cpp
            src/high_pass_filter.cpp
            src/noise_gate.cpp
            src/automatic_gain_control.cpp
)
target_include_directories(test_mic_dsp_audio_src PRIVATE src include)

exosnap_add_gtest(
    NAME mixed_audio_src_tests
    TEST_PREFIX recorder_core.
    SOURCES tests/test_mixed_audio_src.cpp
    LIBRARIES recorder_core
)
target_include_directories(mixed_audio_src_tests PRIVATE src)

exosnap_add_gtest(
    NAME test_session_stats_meter
    TEST_PREFIX recorder_core.
    SOURCES tests/test_session_stats_meter.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_session_stats_meter PRIVATE src)

exosnap_add_gtest(
    NAME test_gpu_compositor
    TEST_PREFIX recorder_core.
    SOURCES tests/test_gpu_compositor.cpp
    LIBRARIES recorder_core d3d11.lib dxgi.lib d3dcompiler.lib
)
target_include_directories(test_gpu_compositor PRIVATE src)
target_compile_definitions(test_gpu_compositor PRIVATE NOMINMAX)

exosnap_add_gtest(
    NAME test_matroska_mux_structure
    TEST_PREFIX recorder_core.
    SOURCES tests/test_matroska_mux_structure.cpp
    LIBRARIES recorder_core EBML::ebml Matroska::matroska
)
target_include_directories(test_matroska_mux_structure PRIVATE src)

exosnap_add_gtest(
    NAME test_matroska_stream_writer
    TEST_PREFIX recorder_core.
    SOURCES tests/test_matroska_stream_writer.cpp
    LIBRARIES recorder_core EBML::ebml Matroska::matroska
)
target_include_directories(test_matroska_stream_writer PRIVATE src)

exosnap_add_gtest(
    NAME test_split_segments
    TEST_PREFIX recorder_core.
    SOURCES tests/test_split_segments.cpp
    LIBRARIES recorder_core EBML::ebml Matroska::matroska
)
target_include_directories(test_split_segments PRIVATE src)

exosnap_add_gtest(
    NAME test_pipeline_diagnostics
    TEST_PREFIX recorder_core.
    SOURCES tests/test_pipeline_diagnostics.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_pipeline_diagnostics PRIVATE src)

# test_mp4_remux_pipeline_flow: pure-logic tests for DeriveTransientMkvPath
# and codec-container gating rules (ADR-0014).  No FFmpeg runtime needed.
exosnap_add_gtest(
    NAME test_mp4_remux_pipeline_flow
    TEST_PREFIX recorder_core.
    SOURCES tests/test_mp4_remux_pipeline_flow.cpp
    LIBRARIES recorder_core
)

# test_mp4_remuxer: needs FFmpeg DLLs at runtime for both test discovery and
# CTest execution.  We copy the four mux-only DLLs next to the test binary via
# a POST_BUILD command so the exe always finds them regardless of PATH.
# PRE_TEST discovery defers test enumeration to ctest time so the DLL copy
# runs first.
add_executable(test_mp4_remuxer tests/test_mp4_remuxer.cpp)
target_link_libraries(test_mp4_remuxer PRIVATE
    GTest::gtest_main
    exosnap::warnings
    recorder_core EBML::ebml Matroska::matroska FFmpeg::mux
)
target_include_directories(test_mp4_remuxer PRIVATE src)
foreach(_ffmpeg_dll IN LISTS EXOSNAP_FFMPEG_DLLS)
    add_custom_command(TARGET test_mp4_remuxer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_ffmpeg_dll}"
            "$<TARGET_FILE_DIR:test_mp4_remuxer>"
        VERBATIM
    )
endforeach()
include(GoogleTest)
# DLL copies above are registered before discovery, so the build-time
# discovery run finds them. PRE_TEST must not be used (see exosnap_testing.cmake).
gtest_discover_tests(test_mp4_remuxer TEST_PREFIX "recorder_core.")
