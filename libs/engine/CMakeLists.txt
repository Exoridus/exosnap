set(_nvenc_header "${CMAKE_SOURCE_DIR}/third_party/nvidia/nvEncodeAPI.h")

if(NOT EXISTS "${_nvenc_header}")
    message(STATUS "exosnap_engine: nvEncodeAPI.h not found at ${_nvenc_header}")
    message(STATUS "  Place nvEncodeAPI.h in third_party/nvidia/ to enable the full exosnap_engine build")
    message(STATUS "  Falling back to skeleton build (version + logging only)")

    add_library(exosnap_engine STATIC
        src/version.cpp
        src/logging/logging.cpp
        src/audio_input_device.cpp
        src/audio_track_model.cpp
        include/exosnap/engine/capture_hub_policy.h
        src/capture_hub_policy.cpp
        src/codec_private.cpp
        include/exosnap/engine/audio_meter.h
        src/audio_meter.cpp
        src/opus_audio_encoder.cpp
        src/pcm_audio_encoder.cpp
        src/flac_audio_encoder.cpp
        src/output_format_audio_src.cpp
        src/output_format_audio_src.h
        src/mic_meter_service.cpp
        src/loopback_meter_service.cpp
        include/exosnap/engine/mp4_remuxer.h
        src/mp4_remuxer.cpp
    )

    target_include_directories(exosnap_engine PUBLIC include)

    set_target_properties(exosnap_engine PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    target_link_libraries(exosnap_engine PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Opus::opus
        FLAC::FLAC
        FFmpeg::mux
        FFmpeg::swresample
        mfuuid.lib
        ole32.lib
        exosnap::warnings
    )

    target_compile_definitions(exosnap_engine PUBLIC EXOSNAP_ENGINE_HAS_WASAPI_CAPTURE_SRC=0)

    exosnap_add_gtest(
        NAME test_audio_track_resolver
        TEST_PREFIX engine.
        SOURCES tests/test_audio_track_resolver.cpp
        LIBRARIES exosnap_engine
    )

    exosnap_add_gtest(
        NAME test_multi_track_packet_types
        TEST_PREFIX engine.
        SOURCES tests/test_multi_track_packet_types.cpp
        LIBRARIES exosnap_engine
    )

    # CV-BUG-001 / CV-BUG-005: ComApartment RAII guard (COM apartment
    # lifetime). Header-only, no exosnap_engine link needed -- just ole32 for
    # the real CoInitializeEx/CoUninitialize calls.
    exosnap_add_gtest(
        NAME test_com_apartment
        TEST_PREFIX engine.
        SOURCES tests/test_com_apartment.cpp
        LIBRARIES ole32.lib
    )
    target_include_directories(test_com_apartment PRIVATE include)

    exosnap_add_gtest(
        NAME test_codec_private_readiness
        TEST_PREFIX engine.
        SOURCES tests/test_codec_private_readiness.cpp
        LIBRARIES exosnap_engine
    )
    target_include_directories(test_codec_private_readiness PRIVATE src)

    # Two-phase worker shutdown wait (worker_join.h): guards against the
    # max-recording-duration defect where the join budget started at recording
    # START instead of STOP. Header-only logic exercised with real std::threads.
    exosnap_add_gtest(
        NAME test_worker_join
        TEST_PREFIX engine.
        SOURCES tests/test_worker_join.cpp
        LIBRARIES exosnap_engine
    )
    target_include_directories(test_worker_join PRIVATE src)

    # Progress-based two-phase shutdown policy (finalize_join_policy.h). Pure logic.
    exosnap_add_gtest(
        NAME test_finalize_join_policy
        TEST_PREFIX engine.
        SOURCES tests/test_finalize_join_policy.cpp
        LIBRARIES exosnap_engine
    )
    target_include_directories(test_finalize_join_policy PRIVATE src)

    exosnap_add_gtest(
        NAME test_opus_encoder
        TEST_PREFIX engine.
        SOURCES tests/test_opus_encoder.cpp
        LIBRARIES exosnap_engine
    )
    target_include_directories(test_opus_encoder PRIVATE src)

    # Audio v2 (0.6.0): PCM passthrough encoder (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_pcm_audio_encoder
        TEST_PREFIX engine.
        SOURCES tests/test_pcm_audio_encoder.cpp
                src/pcm_audio_encoder.cpp
    )
    target_include_directories(test_pcm_audio_encoder PRIVATE src include)

    # Audio v2 (0.6.0): FLAC lossless encoder (links libFLAC; not pure-logic).
    exosnap_add_gtest(
        NAME test_flac_audio_encoder
        TEST_PREFIX engine.
        SOURCES tests/test_flac_audio_encoder.cpp
                src/flac_audio_encoder.cpp
        LIBRARIES FLAC::FLAC
    )
    target_include_directories(test_flac_audio_encoder PRIVATE src include)

    exosnap_add_gtest(
        NAME test_audio_meter
        TEST_PREFIX engine.
        SOURCES tests/test_audio_meter.cpp
        LIBRARIES exosnap_engine
    )

    exosnap_add_gtest(
        NAME test_mic_meter_service
        TEST_PREFIX engine.
        SOURCES tests/test_mic_meter_service.cpp
        LIBRARIES exosnap_engine
    )
    target_include_directories(test_mic_meter_service PRIVATE src)

    # Audio v2 (0.6.0): GainDbToLinear unit tests + per-row gain propagation.
    exosnap_add_gtest(
        NAME test_audio_gain_model
        TEST_PREFIX engine.
        SOURCES tests/test_audio_gain_model.cpp
        LIBRARIES exosnap_engine
    )

    # Audio v2 (0.6.0): brickwall limiter DSP (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_brickwall_limiter
        TEST_PREFIX engine.
        SOURCES tests/test_brickwall_limiter.cpp
                src/brickwall_limiter.cpp
    )
    target_include_directories(test_brickwall_limiter PRIVATE src)

    # Audio v2 (0.6.0): mic high-pass filter DSP (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_high_pass_filter
        TEST_PREFIX engine.
        SOURCES tests/test_high_pass_filter.cpp
                src/high_pass_filter.cpp
    )
    target_include_directories(test_high_pass_filter PRIVATE src)

    # Audio v2 (0.6.0): mic noise gate DSP (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_noise_gate
        TEST_PREFIX engine.
        SOURCES tests/test_noise_gate.cpp
                src/noise_gate.cpp
    )
    target_include_directories(test_noise_gate PRIVATE src)

    # Audio v2 (0.6.0): mic automatic gain control DSP (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_automatic_gain_control
        TEST_PREFIX engine.
        SOURCES tests/test_automatic_gain_control.cpp
                src/automatic_gain_control.cpp
    )
    target_include_directories(test_automatic_gain_control PRIVATE src)

    # Audio v2 (0.6.0): RNNoise neural noise-suppression DSP (links rnnoise).
    exosnap_add_gtest(
        NAME test_rnnoise_denoiser
        TEST_PREFIX engine.
        SOURCES tests/test_rnnoise_denoiser.cpp
                src/rnnoise_denoiser.cpp
        LIBRARIES RNNoise::rnnoise
    )
    target_include_directories(test_rnnoise_denoiser PRIVATE src include)

    # Audio v2 (0.6.0): MicDspAudioSrc decorator (self-contained; no FFmpeg/NVENC).
    exosnap_add_gtest(
        NAME test_mic_dsp_audio_src
        TEST_PREFIX engine.
        SOURCES tests/test_mic_dsp_audio_src.cpp
                src/mic_dsp_audio_src.cpp
                src/high_pass_filter.cpp
                src/noise_gate.cpp
                src/automatic_gain_control.cpp
                src/rnnoise_denoiser.cpp
        LIBRARIES RNNoise::rnnoise
    )
    target_include_directories(test_mic_dsp_audio_src PRIVATE src include)

    # ADR 0030 (0.6.0): OutputFormatAudioSrc resampling decorator (links swresample).
    exosnap_add_gtest(
        NAME test_output_format_audio_src
        TEST_PREFIX engine.
        SOURCES tests/test_output_format_audio_src.cpp
                src/output_format_audio_src.cpp
        LIBRARIES FFmpeg::swresample
    )
    target_include_directories(test_output_format_audio_src PRIVATE src include)

    # test_mp4_remux_pipeline_flow: pure-logic tests; no FFmpeg runtime needed.
    exosnap_add_gtest(
        NAME test_mp4_remux_pipeline_flow
        TEST_PREFIX engine.
        SOURCES tests/test_mp4_remux_pipeline_flow.cpp
        LIBRARIES exosnap_engine
    )

    # 0.7.0 HEVC: hvcC builder — pure-logic, no GPU or NVENC needed.
    exosnap_add_gtest(
        NAME test_annexb_to_hvcc
        TEST_PREFIX engine.
        SOURCES tests/test_annexb_to_hvcc.cpp
                src/annexb_to_hvcc.cpp
    )
    target_include_directories(test_annexb_to_hvcc PRIVATE src)

    # test_mp4_remuxer / test_remux_trim: go through the shared helper like every
    # other binary so they get the same FFmpeg+Qt DLL staging and a single
    # per-binary CTest entry. (Previously hand-rolled with add_executable +
    # POST_BUILD DLL copy + gtest_discover_tests.)
    exosnap_add_gtest(
        NAME test_mp4_remuxer
        TEST_PREFIX engine.
        SOURCES tests/test_mp4_remuxer.cpp
        LIBRARIES exosnap_engine EBML::ebml Matroska::matroska FFmpeg::mux
    )
    target_include_directories(test_mp4_remuxer PRIVATE src)

    # 0.9.0 S1: TrimRange stream-copy + ExtractKeyframeTimestamps tests.
    exosnap_add_gtest(
        NAME test_remux_trim
        TEST_PREFIX engine.
        SOURCES tests/test_remux_trim.cpp
        LIBRARIES exosnap_engine EBML::ebml Matroska::matroska FFmpeg::mux
    )
    target_include_directories(test_remux_trim PRIVATE src)

    return()
endif()

# ---------------------------------------------------------------------------
# Full build — all M3.1 sources
# ---------------------------------------------------------------------------

add_library(exosnap_engine STATIC
    src/version.cpp
    src/logging/logging.cpp

    src/recorder_session.cpp
    src/audio_input_device.cpp
    src/codec_private.cpp
    include/exosnap/engine/audio_meter.h
    src/audio_meter.cpp
    src/wgc_capture.cpp
    src/dxgi_od_capture_src.cpp
    include/exosnap/engine/dxgi_od_capture_src.h
    src/frame_pacing.cpp
    src/nvenc_encoder.cpp
    src/nvenc_video_encoder.cpp
    include/exosnap/engine/interfaces/IVideoEncoder.h
    include/exosnap/engine/interfaces/VideoEncoderFactory.h
    src/video_encoder_factory.cpp
    include/exosnap/engine/hdr_bitstream_metadata.h
    src/hdr_bitstream_metadata.cpp
    src/ffmpeg_aac_encoder.cpp
    src/gpu_compositor.cpp
    src/gpu_compositor.h
    src/cursor_sprite.cpp
    include/exosnap/engine/cursor_sprite.h
    src/gpu_hdr_tonemap.cpp
    include/exosnap/engine/gpu_hdr_tonemap.h
    src/gpu_timestamp_profiler.cpp
    include/exosnap/engine/gpu_timestamp_profiler.h
    include/exosnap/engine/gpu_timestamp_math.h
    src/gpu_rgb_to_ayuv.cpp
    src/gpu_rgb_to_ayuv.h
    src/gpu_hdr_pq.cpp
    src/gpu_hdr_pq.h
    src/hdr_tonemap.h
    src/hdr_pq.h
    src/hdr_reference_white.h
    src/hdr_preview.h
    src/hdr_preview.cpp
    include/exosnap/engine/hdr_native.h
    include/exosnap/engine/preview_tap.h
    src/preview_tap.cpp
    src/mixed_audio_src.cpp
    src/brickwall_limiter.cpp
    src/brickwall_limiter.h
    src/high_pass_filter.cpp
    src/high_pass_filter.h
    src/noise_gate.cpp
    src/noise_gate.h
    src/automatic_gain_control.cpp
    src/automatic_gain_control.h
    src/rnnoise_denoiser.cpp
    src/rnnoise_denoiser.h
    src/mic_dsp_audio_src.cpp
    src/mic_dsp_audio_src.h
    src/output_format_audio_src.cpp
    src/output_format_audio_src.h
    src/wasapi_capture_src.cpp
    include/exosnap/engine/wasapi_audio_render.h
    src/wasapi_audio_render.cpp
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
    include/exosnap/engine/pipeline_diagnostics.h
    include/exosnap/engine/pipeline_health.h
    src/pipeline_health.cpp
    src/video_thread.cpp
    src/yuv_to_bgra.h
    src/yuv_to_bgra.cpp
    src/preview_publish_gate.h
    include/exosnap/engine/preview_shared_texture.h
    src/preview_shared_texture.cpp
    src/audio_thread.cpp
    src/mux_thread.cpp
    src/matroska_stream_writer.cpp
    src/portable_file_io.cpp
    include/exosnap/engine/portable_file_io.h
    src/annexb_to_avcc.cpp
    src/annexb_to_hvcc.cpp
    src/audio_track_model.cpp
    include/exosnap/engine/capture_hub_policy.h
    src/capture_hub_policy.cpp
    src/mic_meter_service.cpp
    src/loopback_meter_service.cpp
    include/exosnap/engine/mp4_remuxer.h
    src/mp4_remuxer.cpp
    include/exosnap/engine/edit_player_engine.h
    src/edit_player_engine.cpp
    src/edit_player_hw_decode.h
    src/edit_player_hw_decode.cpp
    include/exosnap/engine/edit_frame_gpu_converter.h
    src/edit_frame_gpu_converter.cpp
    src/edit_audio_mix.h
    src/edit_audio_mix.cpp
    src/edit_playback_pacing.h
    src/playback_clock.h
    src/playback_clock.cpp
    include/exosnap/engine/edit_player_session.h
    src/edit_player_session.cpp
)

target_include_directories(exosnap_engine
    PUBLIC  include
            # Header-only: VideoEncoderFactory.h needs capability::AdapterVendor
            # (adapter_enum.h, no compiled symbols). Include path only, deliberately
            # NOT a target_link_libraries(exosnap_engine ... exosnap::capability) --
            # exosnap_capability already links exosnap_engine PUBLICLY, so linking it
            # back here would be a build-graph cycle.
            "${CMAKE_SOURCE_DIR}/libs/capability/include"
    PRIVATE src
            "${CMAKE_SOURCE_DIR}/third_party/nvidia"
)

set_target_properties(exosnap_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(exosnap_engine
    PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Opus::opus
        FLAC::FLAC
        RNNoise::rnnoise
        EBML::ebml
        Matroska::matroska
        FFmpeg::mux
        FFmpeg::swresample
        d3d11.lib
        d3dcompiler.lib
        dxgi.lib
        dwmapi.lib
        gdi32.lib
        user32.lib
        runtimeobject.lib
        ole32.lib
        avrt.lib
        mfplat.lib
        mf.lib
        mfuuid.lib
        wmcodecdspuuid.lib
        exosnap::warnings
)

target_compile_definitions(exosnap_engine PUBLIC EXOSNAP_ENGINE_HAS_WASAPI_CAPTURE_SRC=1)

exosnap_add_gtest(
    NAME test_audio_track_resolver
    TEST_PREFIX engine.
    SOURCES tests/test_audio_track_resolver.cpp
    LIBRARIES exosnap_engine
)

# CV-BUG-001 / CV-BUG-005: ComApartment RAII guard (COM apartment lifetime).
# Header-only, no exosnap_engine link needed -- just ole32 for the real
# CoInitializeEx/CoUninitialize calls.
exosnap_add_gtest(
    NAME test_com_apartment
    TEST_PREFIX engine.
    SOURCES tests/test_com_apartment.cpp
    LIBRARIES ole32.lib
)
target_include_directories(test_com_apartment PRIVATE include)

# Structured-log schema: base fields (launch session id) stamped onto every JSONL
# record, per-call fields winning on collision, and size-based file rotation.
exosnap_add_gtest(
    NAME test_logging_schema
    TEST_PREFIX engine.
    SOURCES tests/test_logging_schema.cpp
    LIBRARIES exosnap_engine
)

# Capture-hub arbitration: consumer refcount, the held frame across loss and
# unbounded reopen, and the release-before-the-engine-opens handover order that
# the one-duplication-per-process constraint forces. Pure, GPU-free.
exosnap_add_gtest(
    NAME test_capture_hub_policy
    TEST_PREFIX engine.
    SOURCES tests/test_capture_hub_policy.cpp
    LIBRARIES exosnap_engine
)

# Preview-tap plan: which sessions share their pre-encode surface with the
# preview and which display transform the consumer applies. Pure, GPU-free.
exosnap_add_gtest(
    NAME test_preview_tap_plan
    TEST_PREFIX engine.
    SOURCES tests/test_preview_tap_plan.cpp
    LIBRARIES exosnap_engine
)

# Cursor-sprite placement: the clip/scale arithmetic shared by the recording
# compositor and the preview's cursor sprite. Pure, GPU-free.
exosnap_add_gtest(
    NAME test_cursor_sprite
    TEST_PREFIX engine.
    SOURCES tests/test_cursor_sprite.cpp
    LIBRARIES exosnap_engine
)

exosnap_add_gtest(
    NAME test_multi_track_packet_types
    TEST_PREFIX engine.
    SOURCES tests/test_multi_track_packet_types.cpp
    LIBRARIES exosnap_engine
)

exosnap_add_gtest(
    NAME test_codec_private_readiness
    TEST_PREFIX engine.
    SOURCES tests/test_codec_private_readiness.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_codec_private_readiness PRIVATE src)

# Two-phase worker shutdown wait (worker_join.h): guards against the
# max-recording-duration defect where the join budget started at recording
# START instead of STOP. Header-only logic exercised with real std::threads.
exosnap_add_gtest(
    NAME test_worker_join
    TEST_PREFIX engine.
    SOURCES tests/test_worker_join.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_worker_join PRIVATE src)

exosnap_add_gtest(
    NAME test_opus_encoder
    TEST_PREFIX engine.
    SOURCES tests/test_opus_encoder.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_opus_encoder PRIVATE src)

# Audio v2 (0.6.0): PCM passthrough encoder (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_pcm_audio_encoder
    TEST_PREFIX engine.
    SOURCES tests/test_pcm_audio_encoder.cpp
            src/pcm_audio_encoder.cpp
)
target_include_directories(test_pcm_audio_encoder PRIVATE src include)

# Audio v2 (0.6.0): FLAC lossless encoder (links libFLAC; not pure-logic).
exosnap_add_gtest(
    NAME test_flac_audio_encoder
    TEST_PREFIX engine.
    SOURCES tests/test_flac_audio_encoder.cpp
            src/flac_audio_encoder.cpp
    LIBRARIES FLAC::FLAC
)
target_include_directories(test_flac_audio_encoder PRIVATE src include)

exosnap_add_gtest(
    NAME test_audio_encoding_params
    TEST_PREFIX engine.
    SOURCES tests/test_audio_encoding_params.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_audio_encoding_params PRIVATE src)

exosnap_add_gtest(
    NAME test_audio_thread_source_agnostic
    TEST_PREFIX engine.
    SOURCES tests/test_audio_thread_source_agnostic.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_audio_thread_source_agnostic PRIVATE src)

# Discontinuity gap compensation: the device-position arithmetic that turns a
# WASAPI DATA_DISCONTINUITY into a measured gap, and the AudioThread silence
# fill that keeps the sample timeline (and A/V sync) continuous across it.
exosnap_add_gtest(
    NAME test_audio_discontinuity_gap
    TEST_PREFIX engine.
    SOURCES tests/test_audio_discontinuity_gap.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_audio_discontinuity_gap PRIVATE src)

# Steady-state mux queue bound: a stalled/slow consumer blocks producers and
# times out into a deterministic ErrorPhase::Mux failure instead of unbounded
# RAM growth; drain and failure both wake blocked producers.
exosnap_add_gtest(
    NAME test_mux_queue_backpressure
    TEST_PREFIX engine.
    SOURCES tests/test_mux_queue_backpressure.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_mux_queue_backpressure PRIVATE src)

# Fresh-session stop_requested reset (session_stop_reset.h): a Stop() call that
# races ahead of the next Record() call must not be silently discarded by
# Record()'s own state reset. Pure logic against a plain SessionState — no
# capture hardware, no threads.
exosnap_add_gtest(
    NAME test_session_stop_reset
    TEST_PREFIX engine.
    SOURCES tests/test_session_stop_reset.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_session_stop_reset PRIVATE src)

# Worker teardown ownership: an abandoned (stalled/hung) worker keeps its
# SessionState and capture source alive through the thread's shared ownership,
# wakes on the stop/failure flags, and runs out cleanly — never a detach into
# freed session memory, never a teardown deadlock on the mux queue bound.
exosnap_add_gtest(
    NAME test_worker_teardown_ownership
    TEST_PREFIX engine.
    SOURCES tests/test_worker_teardown_ownership.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_worker_teardown_ownership PRIVATE src)

# FfmpegAacEncoder (ADR 0052): native FFmpeg AAC-LC encoder, the sole AAC path
# now that the prior third-party AAC encoder has been removed. Links
# exosnap_engine (which carries the encoder + FFmpeg::mux/swresample
# transitively); the FFmpeg DLLs are staged next to the test binary by
# exosnap_add_gtest.
exosnap_add_gtest(
    NAME test_ffmpeg_aac_encoder
    TEST_PREFIX engine.
    SOURCES tests/test_ffmpeg_aac_encoder.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_ffmpeg_aac_encoder PRIVATE src)

exosnap_add_gtest(
    NAME test_nvenc_video_encoder_interface
    TEST_PREFIX engine.
    SOURCES tests/test_nvenc_video_encoder_interface.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_nvenc_video_encoder_interface PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

# IVideoEncoder-refactor (encoder-tuning step 1/3): FakeVideoEncoder-backed
# coverage of encoder dispatch paths video_thread.cpp only exercised via real
# NVENC hardware before. Placeholder body on the parallel-split base commit;
# Agent B fills it in alongside tests/fakes/fake_video_encoder.h.
exosnap_add_gtest(
    NAME test_video_thread_encoder_dispatch
    TEST_PREFIX engine.
    SOURCES tests/test_video_thread_encoder_dispatch.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_video_thread_encoder_dispatch PRIVATE src)

# test_nvenc_rc_params: pure unit tests for ComputeNvencRcParams (no GPU needed).
exosnap_add_gtest(
    NAME test_nvenc_rc_params
    TEST_PREFIX engine.
    SOURCES tests/test_nvenc_rc_params.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_nvenc_rc_params PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

# test_nvenc_color_config: pure unit tests for ApplyColorMetadataToNvenc
# (fix for color-range-signaling bug — no GPU needed).
exosnap_add_gtest(
    NAME test_nvenc_color_config
    TEST_PREFIX engine.
    SOURCES tests/test_nvenc_color_config.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_nvenc_color_config PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

# test_nvenc_chroma_config: pure unit tests for the NVENC chroma/input-format
# helpers backing the 4:4:4 expert path (no GPU needed).
exosnap_add_gtest(
    NAME test_nvenc_chroma_config
    TEST_PREFIX engine.
    SOURCES tests/test_nvenc_chroma_config.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_nvenc_chroma_config PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

# test_nvenc_gop_aq_config: pure unit tests for the GOP + adaptive-quantization
# helpers backing the keyframe-interval selector and the explicit spatial-AQ
# setting — ComputeGopLength / ApplyGopToNvenc / ApplySpatialAqToNvenc /
# NextGopKeyframePhase (no GPU needed).
exosnap_add_gtest(
    NAME test_nvenc_gop_aq_config
    TEST_PREFIX engine.
    SOURCES tests/test_nvenc_gop_aq_config.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_nvenc_gop_aq_config PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

# test_split_sentinel_policy: pure unit tests for ShouldEmitSplitSentinel, the
# split-boundary routing condition hardened for async submit-ahead — no
# GPU needed.
exosnap_add_gtest(
    NAME test_split_sentinel_policy
    TEST_PREFIX engine.
    SOURCES tests/test_split_sentinel_policy.cpp
)
target_include_directories(test_split_sentinel_policy PRIVATE src)

# test_y4m_reader: pure unit tests for the YUV4MPEG2 header/frame parser
# backing the encoder quality matrix harness's probe (no GPU needed).
exosnap_add_gtest(
    NAME test_y4m_reader
    TEST_PREFIX engine.
    SOURCES tests/test_y4m_reader.cpp src/y4m_reader.cpp
)
target_include_directories(test_y4m_reader PRIVATE src)

# test_yuv_convert: pure unit tests for I420->NV12 conversion backing the
# encoder quality matrix harness's probe (no GPU needed).
exosnap_add_gtest(
    NAME test_yuv_convert
    TEST_PREFIX engine.
    SOURCES tests/test_yuv_convert.cpp src/yuv_convert.cpp
)
target_include_directories(test_yuv_convert PRIVATE src)

# test_elementary_stream_writer: pure unit tests for IVF header framing
# (AV1 elementary-stream output) backing the encoder quality matrix harness's
# probe (no GPU needed).
exosnap_add_gtest(
    NAME test_elementary_stream_writer
    TEST_PREFIX engine.
    SOURCES tests/test_elementary_stream_writer.cpp src/elementary_stream_writer.cpp
)
target_include_directories(test_elementary_stream_writer PRIVATE src)

# test_nvenc_output_ring: pure unit tests for the async output-ring free-slot
# scan (FindFreeOutputSlot) backing the async submit path's output-buffer
# allocation (no GPU needed).
exosnap_add_gtest(
    NAME test_nvenc_output_ring
    TEST_PREFIX engine.
    SOURCES tests/test_nvenc_output_ring.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_nvenc_output_ring PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

# test_hdr_bitstream_metadata: byte-exact unit tests for the in-band HDR10
# metadata payload builders (HEVC SEI 137/144, AV1 MDCV/CLL OBU payloads). Pure
# functions — no GPU/NVENC.
exosnap_add_gtest(
    NAME test_hdr_bitstream_metadata
    TEST_PREFIX engine.
    SOURCES tests/test_hdr_bitstream_metadata.cpp
            src/hdr_bitstream_metadata.cpp
)
target_include_directories(test_hdr_bitstream_metadata PRIVATE src include)

# test_hdr_native_reconcile: coordinator-seam tests for the native HDR10 encode
# reconcile (ApplyHdr10NativeEncode) — pins 10-bit and snaps 4:4:4 -> 4:2:0 so an
# HDR-active display never turns a valid UI selection into a Validate() failure.
exosnap_add_gtest(
    NAME test_hdr_native_reconcile
    TEST_PREFIX engine.
    SOURCES tests/test_hdr_native_reconcile.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_hdr_native_reconcile PRIVATE src include)

# test_nvenc_preset_guid: pure unit tests for NvencPresetToGuid (P1..P7 -> SDK
# preset GUID mapping — no GPU needed).
exosnap_add_gtest(
    NAME test_nvenc_preset_guid
    TEST_PREFIX engine.
    SOURCES tests/test_nvenc_preset_guid.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_nvenc_preset_guid PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

# test_nvenc_flush_drain_policy: pure bounded flush-drain policy (never wedge on a
# lost/hung device — abort the drain past the time budget). No GPU needed.
exosnap_add_gtest(
    NAME test_nvenc_flush_drain_policy
    TEST_PREFIX engine.
    SOURCES tests/test_nvenc_flush_drain_policy.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_nvenc_flush_drain_policy PRIVATE src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia")

# test_finalize_join_policy: pure two-phase shutdown policy — a progress-based
# finalize wait (never truncate a slow-but-progressing finalize; abort only on a
# real byte stall) and the Phase-1/Phase-2 fault separation. No GPU / no threads.
exosnap_add_gtest(
    NAME test_finalize_join_policy
    TEST_PREFIX engine.
    SOURCES tests/test_finalize_join_policy.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_finalize_join_policy PRIVATE src)

exosnap_add_gtest(
    NAME test_mic_channel_mapping
    TEST_PREFIX engine.
    SOURCES tests/test_mic_channel_mapping.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_mic_channel_mapping PRIVATE src)

exosnap_add_gtest(
    NAME test_audio_plan_validate
    TEST_PREFIX engine.
    SOURCES tests/test_audio_plan_validate.cpp
    LIBRARIES exosnap_engine
)

exosnap_add_gtest(
    NAME test_webm_container_validation
    TEST_PREFIX engine.
    SOURCES tests/test_webm_container_validation.cpp
    LIBRARIES exosnap_engine
)

exosnap_add_gtest(
    NAME test_mp4_container_validation
    TEST_PREFIX engine.
    SOURCES tests/test_mp4_container_validation.cpp
    LIBRARIES exosnap_engine
)

exosnap_add_gtest(
    NAME test_annexb_to_avcc
    TEST_PREFIX engine.
    SOURCES tests/test_annexb_to_avcc.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_annexb_to_avcc PRIVATE src)

# 0.7.0 HEVC: hvcC (HEVCDecoderConfigurationRecord) builder from Annex-B VPS/SPS/PPS.
exosnap_add_gtest(
    NAME test_annexb_to_hvcc
    TEST_PREFIX engine.
    SOURCES tests/test_annexb_to_hvcc.cpp
            src/annexb_to_hvcc.cpp
)
target_include_directories(test_annexb_to_hvcc PRIVATE src)

exosnap_add_gtest(
    NAME test_audio_source_semantics
    TEST_PREFIX engine.
    SOURCES tests/test_audio_source_semantics.cpp
    LIBRARIES exosnap_engine
)

exosnap_add_gtest(
    NAME test_audio_duration_stats
    TEST_PREFIX engine.
    SOURCES tests/test_audio_duration_stats.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_audio_duration_stats PRIVATE src)

# The recording clock's pause arithmetic. Reaches into src/ for the collector's
# header, which is where the one function that answers "how long is this
# recording" lives.
exosnap_add_gtest(
    NAME test_captured_seconds
    TEST_PREFIX engine.
    SOURCES tests/test_captured_seconds.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_captured_seconds PRIVATE src)

exosnap_add_gtest(
    NAME test_audio_meter
    TEST_PREFIX engine.
    SOURCES tests/test_audio_meter.cpp
    LIBRARIES exosnap_engine
)

exosnap_add_gtest(
    NAME test_mic_meter_service
    TEST_PREFIX engine.
    SOURCES tests/test_mic_meter_service.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_mic_meter_service PRIVATE src)

# Audio v2 (0.6.0): GainDbToLinear unit tests + per-row gain propagation.
exosnap_add_gtest(
    NAME test_audio_gain_model
    TEST_PREFIX engine.
    SOURCES tests/test_audio_gain_model.cpp
    LIBRARIES exosnap_engine
)

# Audio v2 (0.6.0): brickwall limiter DSP (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_brickwall_limiter
    TEST_PREFIX engine.
    SOURCES tests/test_brickwall_limiter.cpp
            src/brickwall_limiter.cpp
)
target_include_directories(test_brickwall_limiter PRIVATE src)

# Audio v2 (0.6.0): mic high-pass filter DSP (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_high_pass_filter
    TEST_PREFIX engine.
    SOURCES tests/test_high_pass_filter.cpp
            src/high_pass_filter.cpp
)
target_include_directories(test_high_pass_filter PRIVATE src)

# Audio v2 (0.6.0): mic noise gate DSP (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_noise_gate
    TEST_PREFIX engine.
    SOURCES tests/test_noise_gate.cpp
            src/noise_gate.cpp
)
target_include_directories(test_noise_gate PRIVATE src)

# Audio v2 (0.6.0): mic automatic gain control DSP (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_automatic_gain_control
    TEST_PREFIX engine.
    SOURCES tests/test_automatic_gain_control.cpp
            src/automatic_gain_control.cpp
)
target_include_directories(test_automatic_gain_control PRIVATE src)

# Audio v2 (0.6.0): RNNoise neural noise-suppression DSP (links rnnoise; not
# pure-logic — exercises the real RNNoise default model).
exosnap_add_gtest(
    NAME test_rnnoise_denoiser
    TEST_PREFIX engine.
    SOURCES tests/test_rnnoise_denoiser.cpp
            src/rnnoise_denoiser.cpp
    LIBRARIES RNNoise::rnnoise
)
target_include_directories(test_rnnoise_denoiser PRIVATE src include)

# Audio v2 (0.6.0): MicDspAudioSrc decorator (self-contained; no FFmpeg/NVENC).
exosnap_add_gtest(
    NAME test_mic_dsp_audio_src
    TEST_PREFIX engine.
    SOURCES tests/test_mic_dsp_audio_src.cpp
            src/mic_dsp_audio_src.cpp
            src/high_pass_filter.cpp
            src/noise_gate.cpp
            src/automatic_gain_control.cpp
            src/rnnoise_denoiser.cpp
    LIBRARIES RNNoise::rnnoise
)
target_include_directories(test_mic_dsp_audio_src PRIVATE src include)

# ADR 0030 (0.6.0): OutputFormatAudioSrc resampling decorator (links swresample).
exosnap_add_gtest(
    NAME test_output_format_audio_src
    TEST_PREFIX engine.
    SOURCES tests/test_output_format_audio_src.cpp
            src/output_format_audio_src.cpp
    LIBRARIES FFmpeg::swresample
)
target_include_directories(test_output_format_audio_src PRIVATE src include)

exosnap_add_gtest(
    NAME mixed_audio_src_tests
    TEST_PREFIX engine.
    SOURCES tests/test_mixed_audio_src.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(mixed_audio_src_tests PRIVATE src)

exosnap_add_gtest(
    NAME test_session_stats_meter
    TEST_PREFIX engine.
    SOURCES tests/test_session_stats_meter.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_session_stats_meter PRIVATE src)

exosnap_add_gtest(
    NAME test_gpu_compositor
    TEST_PREFIX engine.
    SOURCES tests/test_gpu_compositor.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib d3dcompiler.lib
)
target_include_directories(test_gpu_compositor PRIVATE src)
target_compile_definitions(test_gpu_compositor PRIVATE NOMINMAX)

# The overlay shader as the DXGI preview uses it: compiled standalone, no compositor.
exosnap_add_gtest(
    NAME test_overlay_shader
    TEST_PREFIX engine.
    SOURCES tests/test_overlay_shader.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib d3dcompiler.lib
)
target_compile_definitions(test_overlay_shader PRIVATE NOMINMAX)

# HDR tone-map (scRGB FP16 -> SDR BT.709): pure curve math + OD capture-format x
# HDR-mode negotiation. No GPU — the compute shader replicates the same math.
exosnap_add_gtest(
    NAME test_hdr_tonemap
    TEST_PREFIX engine.
    SOURCES tests/test_hdr_tonemap.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib
)
target_include_directories(test_hdr_tonemap PRIVATE src)
target_compile_definitions(test_hdr_tonemap PRIVATE NOMINMAX)

# OD acquire-failure classifier: pure recording-loss policy (ACCESS_LOST recover
# vs DEVICE_REMOVED / unexpected HRESULT fail-closed). No GPU.
exosnap_add_gtest(
    NAME test_od_acquire_failure_classify
    TEST_PREFIX engine.
    SOURCES tests/test_od_acquire_failure_classify.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib
)
target_include_directories(test_od_acquire_failure_classify PRIVATE src)
target_compile_definitions(test_od_acquire_failure_classify PRIVATE NOMINMAX)

# WASAPI acquire-failure classifier: pure recording-loss policy for the audio
# drain (endpoint invalidated / audio service down / unexpected HRESULT ->
# fail-closed; benign empty tick -> keep draining). No audio hardware.
exosnap_add_gtest(
    NAME test_wasapi_acquire_failure_classify
    TEST_PREFIX engine.
    SOURCES tests/test_wasapi_acquire_failure_classify.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_wasapi_acquire_failure_classify PRIVATE src)
target_compile_definitions(test_wasapi_acquire_failure_classify PRIVATE NOMINMAX)

# WASAPI loopback acquire classifier: the SYS/system-audio mirror of the above
# for WasapiLoopback/WasapiLoopbackSrc (screen-capture system audio) — pins
# that a device-invalidated GetBuffer/GetNextPacketSize is reported as a fatal
# capture loss instead of the track silently going mute. No audio hardware.
exosnap_add_gtest(
    NAME test_wasapi_loopback_acquire_classify
    TEST_PREFIX engine.
    SOURCES tests/test_wasapi_loopback_acquire_classify.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_wasapi_loopback_acquire_classify PRIVATE src)
target_compile_definitions(test_wasapi_loopback_acquire_classify PRIVATE NOMINMAX)

# OD reopen policy: pure live re-duplication retry loop (recovered continue vs
# in-budget retry vs budget-exhausted give-up/clean-stop). No GPU.
exosnap_add_gtest(
    NAME test_od_reopen_policy
    TEST_PREFIX engine.
    SOURCES tests/test_od_reopen_policy.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib
)
target_include_directories(test_od_reopen_policy PRIVATE src)
target_compile_definitions(test_od_reopen_policy PRIVATE NOMINMAX)

# First-frame wait guard (S3): the 5 s first-frame timeout is suspended while an
# OD start-hold recovers a fullscreen/mode transition, and the 15 s reopen budget
# owns the deadline. Pure; no GPU.
exosnap_add_gtest(
    NAME test_first_frame_wait_step
    TEST_PREFIX engine.
    SOURCES tests/test_first_frame_wait_step.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib
)
target_include_directories(test_first_frame_wait_step PRIVATE src)
target_compile_definitions(test_first_frame_wait_step PRIVATE NOMINMAX)

# Audio device-loss policy (ADR 0046): pure classifier (endpoint loss ->
# degrade the source, not kill the session), unbounded reactivation cadence, and
# the PID-reuse identity guard for process-loopback flavors. No audio hardware.
exosnap_add_gtest(
    NAME test_audio_device_loss_policy
    TEST_PREFIX engine.
    SOURCES tests/test_audio_device_loss_policy.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_audio_device_loss_policy PRIVATE src)
target_compile_definitions(test_audio_device_loss_policy PRIVATE NOMINMAX)

# MixedAudioSrc per-source degradation (ADR 0046): a lost inner source degrades
# to honest silence (visible via DegradedSourceCount) while the survivors keep
# mixing, and Reinit reacquires it. No audio hardware.
exosnap_add_gtest(
    NAME test_mixed_audio_device_loss
    TEST_PREFIX engine.
    SOURCES tests/test_mixed_audio_device_loss.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_mixed_audio_device_loss PRIVATE src)
target_compile_definitions(test_mixed_audio_device_loss PRIVATE NOMINMAX)

# AudioThread device-loss integration (ADR 0046): a fake source that loses its
# endpoint mid-recording keeps the session alive (no RecordFailure), holds the
# timeline with silence, reactivates via Reinit, and still emits EOS; a merged
# track survives one dead inner. No audio hardware.
exosnap_add_gtest(
    NAME test_audio_thread_device_loss
    TEST_PREFIX engine.
    SOURCES tests/test_audio_thread_device_loss.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_audio_thread_device_loss PRIVATE src)
target_compile_definitions(test_audio_thread_device_loss PRIVATE NOMINMAX)

# Silent-stall fill: a healthy source that stops delivering packets (WASAPI
# loopback while nothing is playing) must not freeze the encoder timeline, and
# a stall the device also reports must not be filled twice. No audio hardware.
exosnap_add_gtest(
    NAME test_audio_thread_silence_fill
    TEST_PREFIX engine.
    SOURCES tests/test_audio_thread_silence_fill.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_audio_thread_silence_fill PRIVATE src)
target_compile_definitions(test_audio_thread_silence_fill PRIVATE NOMINMAX)
# The fill is judged against wall time with an 80 ms tolerance, so a poll thread
# that loses the CPU to fifteen sibling test processes reads as silence the
# engine failed to fill. Serial rather than a wider tolerance: the tolerance is
# what would otherwise stop noticing a real lag.
set_tests_properties(engine.test_audio_thread_silence_fill PROPERTIES RUN_SERIAL TRUE)

# Wall-clock silence-fill arithmetic (audio_silence_fill.h) — pure policy.
exosnap_add_gtest(
    NAME test_audio_silence_fill
    TEST_PREFIX engine.
    SOURCES tests/test_audio_silence_fill.cpp
)
target_include_directories(test_audio_silence_fill PRIVATE src)

# A/V epoch alignment (av_epoch_align.h): where an audio track starts relative
# to video, and how that shifts its packet timestamps — pure policy.
exosnap_add_gtest(
    NAME test_av_epoch_align
    TEST_PREFIX engine.
    SOURCES tests/test_av_epoch_align.cpp
)
target_include_directories(test_av_epoch_align PRIVATE src)

# Capture drain selection: the frame loop must never drain the backend it does not
# own. Pins that an OD capture mid-reopen drains nothing rather than the null WGC pool.
exosnap_add_gtest(
    NAME test_capture_drain_step
    TEST_PREFIX engine.
    SOURCES tests/test_capture_drain_step.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib
)
target_include_directories(test_capture_drain_step PRIVATE src)
target_compile_definitions(test_capture_drain_step PRIVATE NOMINMAX)

# HDR tone-map render pass on the GPU: WARP-backed proof that the shader renders
# the curve correctly into both the 8-bit (BGRA8) and 10-bit (R10G10B10A2)
# intermediates, and that the 10-bit target preserves precision the 8-bit loses.
exosnap_add_gtest(
    NAME test_gpu_hdr_tonemap
    TEST_PREFIX engine.
    SOURCES tests/test_gpu_hdr_tonemap.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib d3dcompiler.lib
)
target_include_directories(test_gpu_hdr_tonemap PRIVATE src)
target_compile_definitions(test_gpu_hdr_tonemap PRIVATE NOMINMAX)

# RGB -> packed AYUV (4:4:4, 8-bit) conversion on the GPU: WARP-backed golden
# proof that the compute shader emits the exact BT.709 limited/full-range AYUV
# bytes NVENC consumes for YUV444 H.264/HEVC encoding.
exosnap_add_gtest(
    NAME test_gpu_rgb_to_ayuv
    TEST_PREFIX engine.
    SOURCES tests/test_gpu_rgb_to_ayuv.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib d3dcompiler.lib
)
target_include_directories(test_gpu_rgb_to_ayuv PRIVATE src)
target_compile_definitions(test_gpu_rgb_to_ayuv PRIVATE NOMINMAX)

# Native HDR10 (PQ / BT.2020 / P010) output: pure curve + matrix + Y'CbCr math,
# effective-mode decision, and colour-metadata assembly. No GPU — the compute
# shader (gpu_hdr_pq.*) replicates the same math.
exosnap_add_gtest(
    NAME test_hdr_pq
    TEST_PREFIX engine.
    SOURCES tests/test_hdr_pq.cpp
    LIBRARIES exosnap_engine d3d11.lib dxgi.lib
)
target_include_directories(test_hdr_pq PRIVATE src)
target_compile_definitions(test_hdr_pq PRIVATE NOMINMAX)

# HDR overlay reference white: pure conversion/fallback logic for scaling SDR
# overlay sprites to the display's SDR content brightness on the native-HDR10
# path. No GPU/D3D needed (the DisplayConfig query itself lives in
# dxgi_od_capture_src.cpp, exercised indirectly via test_gpu_compositor).
exosnap_add_gtest(
    NAME test_sdr_white_level
    TEST_PREFIX engine.
    SOURCES tests/test_sdr_white_level.cpp
    LIBRARIES exosnap_engine
)

# On-screen monitoring decode for native HDR10 (P010 PQ/BT.2020 -> tone-mapped
# SDR BGRA): pure curve/matrix math shared with the encode reference, plus the
# 10-bit native-HDR guard. No GPU.
exosnap_add_gtest(
    NAME test_hdr_preview
    TEST_PREFIX engine.
    SOURCES tests/test_hdr_preview.cpp
            src/hdr_preview.cpp
)
target_include_directories(test_hdr_preview PRIVATE src include)
target_compile_definitions(test_hdr_preview PRIVATE NOMINMAX)

# Strand 3 slice 1 (WYSIWYG preview): pure NV12/P010 -> BGRA color-conversion
# math. Self-contained (no D3D11/NVENC) -- also the bugfix validation for the
# pre-existing hard-coded-BT.601 snapshot conversion.
exosnap_add_gtest(
    NAME test_yuv_to_bgra
    TEST_PREFIX engine.
    SOURCES tests/test_yuv_to_bgra.cpp
            src/yuv_to_bgra.cpp
)
target_include_directories(test_yuv_to_bgra PRIVATE src include)

# Strand 3 slice 1: conversion micro-benchmark (prints ms/frame at 1080p/4K;
# asserts nothing about time -- CI-safe, numbers go to the engineering record).
exosnap_add_gtest(
    NAME test_yuv_to_bgra_bench
    TEST_PREFIX engine.
    SOURCES tests/test_yuv_to_bgra_bench.cpp
            src/yuv_to_bgra.cpp
)
target_include_directories(test_yuv_to_bgra_bench PRIVATE src include)

# Pure throttle/dirty state machine for the live preview tap. Header-only
# logic; no clock/D3D11/threads needed to test it.
exosnap_add_gtest(
    NAME test_preview_publish_gate
    TEST_PREFIX engine.
    SOURCES tests/test_preview_publish_gate.cpp
)
target_include_directories(test_preview_publish_gate PRIVATE src)

# WasapiAudioRenderer lifecycle safety (construct/destruct/push/stop without a
# real device session). No hardware in CI -- real render is user-live-verified.
exosnap_add_gtest(
    NAME test_wasapi_audio_render
    TEST_PREFIX engine.
    SOURCES tests/test_wasapi_audio_render.cpp
            src/wasapi_audio_render.cpp
    LIBRARIES exosnap_engine FFmpeg::swresample avrt.lib
)
target_include_directories(test_wasapi_audio_render PRIVATE src include)

# Pure playback-clock math for the Edit-page video player: audio-clock derivation
# from rendered-sample count, and video-frame selection/drop-counting against
# that clock. No FFmpeg/COM/Qt.
exosnap_add_gtest(
    NAME test_playback_clock
    TEST_PREFIX engine.
    SOURCES tests/test_playback_clock.cpp
            src/playback_clock.cpp
)
target_include_directories(test_playback_clock PRIVATE src)

# EditPlayerEngine: file-open, stream discovery and error paths. Most of the
# fixtures here are synthetic containers written with libavformat carrying no
# real compressed bitstream -- this FFmpeg build ships no video encoder (LGPL,
# ADR 0007) -- so those exercise Open()/track-discovery only. A handful of
# tests actually decode (DecodeFrameAtRaw/StartPlaybackDecode), which needs
# a real bitstream; those open a checked-in-locally (gitignored, .workspace/ is
# scratch) fixture clip via EXOSNAP_SOURCE_DIR and GTEST_SKIP when it is not
# present on the host, same convention as test_analyze_encode_perf.cpp.
# FFmpeg::mux is listed explicitly because the test compiles
# src/edit_player_engine.cpp itself and needs the libavformat/libavcodec
# headers at compile time (exosnap_engine links FFmpeg PRIVATE, so its include
# dirs don't propagate).
exosnap_add_gtest(
    NAME test_edit_player_engine
    TEST_PREFIX engine.
    SOURCES tests/test_edit_player_engine.cpp
            src/edit_player_engine.cpp
            src/edit_player_hw_decode.cpp
            src/edit_audio_mix.cpp
            src/yuv_to_bgra.cpp
    LIBRARIES exosnap_engine FFmpeg::mux
)
target_include_directories(test_edit_player_engine PRIVATE src include)
target_compile_definitions(test_edit_player_engine PRIVATE EXOSNAP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Hardware-decode readback: semi-planar (NV12/P010) -> planar de-interleave,
# including P010's <<6 left-justification rescale
# (docs/superpowers/specs/2026-08-03-editor-playback-hw-decode-design.md).
# Pure logic on hand-built synthetic frames -- no D3D11 adapter, no decoder,
# no fixture clip needed to run these.
exosnap_add_gtest(
    NAME test_edit_player_hw_decode
    TEST_PREFIX engine.
    SOURCES tests/test_edit_player_hw_decode.cpp
            src/edit_player_hw_decode.cpp
    LIBRARIES FFmpeg::mux
)
target_include_directories(test_edit_player_hw_decode PRIVATE src include)

# EditAudioMixer: how several audio tracks of one recording are summed and how
# the result behaves when it overloads or when one track stops. Self-contained
# (no FFmpeg, no device) -- the whole reason the mix lives outside the engine.
exosnap_add_gtest(
    NAME test_edit_audio_mix
    TEST_PREFIX engine.
    SOURCES tests/test_edit_audio_mix.cpp
            src/edit_audio_mix.cpp
            src/brickwall_limiter.cpp
)
target_include_directories(test_edit_audio_mix PRIVATE src)

# EditPlayerSession: the top-level orchestrator's lifecycle safety (no real
# file/device touched -- real playback is user-live-verified, see the plan's
# Task 11). Links the full exosnap_engine library, which now includes
# edit_player_session.cpp itself.
exosnap_add_gtest(
    NAME test_edit_player_session
    TEST_PREFIX engine.
    SOURCES tests/test_edit_player_session.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_edit_player_session PRIVATE src include)

# EditFrameGpuConverter (editor-playback GPU render path): WARP-backed proof that
# the YUV->BGRA shader renders every DecodedPixelFormat exactly like the CPU
# converters (yuv_to_bgra.cpp) for both the BT.709 and BT.601 matrices, and that
# the HDR10 PQ tone-map shader renders the monitoring chain (hdr_preview.cpp) at
# the session's display peak.
exosnap_add_gtest(
    NAME test_edit_frame_gpu_converter
    TEST_PREFIX engine.
    SOURCES tests/test_edit_frame_gpu_converter.cpp
            src/edit_frame_gpu_converter.cpp
            src/yuv_to_bgra.cpp
            src/hdr_preview.cpp
    LIBRARIES d3d11.lib dxgi.lib d3dcompiler.lib
)
target_include_directories(test_edit_frame_gpu_converter PRIVATE src include)
target_compile_definitions(test_edit_frame_gpu_converter PRIVATE NOMINMAX)

# Producer-side shared GPU texture for the WYSIWYG preview tap: create the
# NT-handle keyed-mutex texture, publish a synthetic frame, open the handle on
# a second D3D11 device and verify the pixels round-trip; plus the non-blocking
# drop discipline. WARP-backed (see test_gpu_compositor above), deterministic.
exosnap_add_gtest(
    NAME test_preview_shared_texture
    TEST_PREFIX engine.
    SOURCES tests/test_preview_shared_texture.cpp
            src/preview_shared_texture.cpp
    LIBRARIES d3d11.lib dxgi.lib
)
target_include_directories(test_preview_shared_texture PRIVATE src include)
target_compile_definitions(test_preview_shared_texture PRIVATE NOMINMAX)

exosnap_add_gtest(
    NAME test_matroska_mux_structure
    TEST_PREFIX engine.
    SOURCES tests/test_matroska_mux_structure.cpp
    LIBRARIES exosnap_engine EBML::ebml Matroska::matroska
)
target_include_directories(test_matroska_mux_structure PRIVATE src)

exosnap_add_gtest(
    NAME test_matroska_stream_writer
    TEST_PREFIX engine.
    SOURCES tests/test_matroska_stream_writer.cpp
    LIBRARIES exosnap_engine EBML::ebml Matroska::matroska FFmpeg::mux
)
target_include_directories(test_matroska_stream_writer PRIVATE src)

# 64-bit-safe file position (fixes: recordings past 2 GiB lost their Matroska
# trailer because std::ftell/fseek use a 32-bit `long` on Windows). Pure CRT
# wrapper, no EBML/Matroska/D3D dependency.
exosnap_add_gtest(
    NAME test_portable_file_io
    TEST_PREFIX engine.
    SOURCES tests/test_portable_file_io.cpp
            src/portable_file_io.cpp
)
target_include_directories(test_portable_file_io PRIVATE include)

exosnap_add_gtest(
    NAME test_split_segments
    TEST_PREFIX engine.
    SOURCES tests/test_split_segments.cpp
    LIBRARIES exosnap_engine EBML::ebml Matroska::matroska
)
target_include_directories(test_split_segments PRIVATE src)

exosnap_add_gtest(
    NAME test_pipeline_diagnostics
    TEST_PREFIX engine.
    SOURCES tests/test_pipeline_diagnostics.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_pipeline_diagnostics PRIVATE src)

# A failed per-packet Annex-B -> avcC/hvcC conversion must fail the recording
# instead of writing samples that contradict the track's CodecPrivate.
exosnap_add_gtest(
    NAME test_mux_annexb_conversion_failure
    TEST_PREFIX engine.
    SOURCES tests/test_mux_annexb_conversion_failure.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_mux_annexb_conversion_failure PRIVATE src)

# A mux stopped before the encoders produced their headers must exit quietly:
# it is the first worker to notice a missing cause, never the cause itself.
exosnap_add_gtest(
    NAME test_mux_start_without_headers
    TEST_PREFIX engine.
    SOURCES tests/test_mux_start_without_headers.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_mux_start_without_headers PRIVATE src)

# A session that never captured a frame must name the capture (or the stop that
# beat it), not the first consumer that missed what the capture never produced.
exosnap_add_gtest(
    NAME test_session_outcome
    TEST_PREFIX engine.
    SOURCES tests/test_session_outcome.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_session_outcome PRIVATE src)

# Pure GPU-timestamp resolution math (tick->ms, disjoint / wrap / uncalibrated
# handling) behind the real-GPU-time pipeline stages. No D3D11 dependency.
exosnap_add_gtest(
    NAME test_gpu_timestamp
    TEST_PREFIX engine.
    SOURCES tests/test_gpu_timestamp.cpp
    LIBRARIES exosnap_engine
)

# A/V clock-drift estimation: the pure device-position/QPC math behind the
# honest av_drift_ms metric (audio device clock vs the QPC video timeline).
exosnap_add_gtest(
    NAME test_audio_clock_drift
    TEST_PREFIX engine.
    SOURCES tests/test_audio_clock_drift.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_audio_clock_drift PRIVATE src)

# A/V clock slaving: the pure P-controller that pulls the audio output timeline
# onto the QPC video axis once measured drift crosses the engage threshold.
exosnap_add_gtest(
    NAME test_clock_slaving
    TEST_PREFIX engine.
    SOURCES tests/test_clock_slaving.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_clock_slaving PRIVATE src)

# test_mp4_remux_pipeline_flow: pure-logic tests for DeriveTransientMkvPath
# and codec-container gating rules (ADR-0014).  No FFmpeg runtime needed.
exosnap_add_gtest(
    NAME test_mp4_remux_pipeline_flow
    TEST_PREFIX engine.
    SOURCES tests/test_mp4_remux_pipeline_flow.cpp
    LIBRARIES exosnap_engine
)

# test_mp4_remuxer: needs FFmpeg DLLs at runtime for CTest execution. The shared
# helper stages the mux DLLs (and Qt) next to the binary and registers a single
# per-binary CTest entry, so the hand-rolled add_executable + POST_BUILD DLL copy
# + gtest_discover_tests is no longer needed.
exosnap_add_gtest(
    NAME test_mp4_remuxer
    TEST_PREFIX engine.
    SOURCES tests/test_mp4_remuxer.cpp
    LIBRARIES exosnap_engine EBML::ebml Matroska::matroska FFmpeg::mux
)
target_include_directories(test_mp4_remuxer PRIVATE src)

# 0.9.0 S1: TrimRange stream-copy + ExtractKeyframeTimestamps tests.
exosnap_add_gtest(
    NAME test_remux_trim
    TEST_PREFIX engine.
    SOURCES tests/test_remux_trim.cpp
    LIBRARIES exosnap_engine EBML::ebml Matroska::matroska FFmpeg::mux
)
target_include_directories(test_remux_trim PRIVATE src)

# Deterministic, GPU-free session driver shared by the e2e test, the soak twin,
# and the recovery drills. It drives the REAL AudioThread + MuxThread +
# MatroskaStreamWriter + Finalize with an in-process video feeder (VideoThread's
# SessionState role). PUBLIC-linking exosnap_engine + FFmpeg/EBML/Matroska so
# consumers inherit them; PRIVATE `src` for the internal worker/state headers.
add_library(exosnap_engine_testutil STATIC
    testutil/synthetic_session.cpp
)
target_include_directories(exosnap_engine_testutil PUBLIC testutil)
target_include_directories(exosnap_engine_testutil PRIVATE src)
target_link_libraries(exosnap_engine_testutil PUBLIC
    exosnap_engine EBML::ebml Matroska::matroska FFmpeg::mux)
target_link_libraries(exosnap_engine_testutil PRIVATE exosnap::warnings)
target_compile_definitions(exosnap_engine_testutil PRIVATE NOMINMAX)
target_compile_features(exosnap_engine_testutil PUBLIC cxx_std_20)

# Whole-pipeline end-to-end: drive the shared synthetic session to a real MKV, then
# validate it with the vendored libavformat (open, read stream info, demux to a
# clean EOF, check tracks/keyframes/monotone PTS) and remux MKV->MP4 through the
# real remuxer. No GPU: the in-lib seam plays VideoThread's SessionState role.
# Not `live` — fully deterministic and device-free.
exosnap_add_gtest(
    NAME test_session_e2e_real_file
    TEST_PREFIX engine.
    SOURCES tests/test_session_e2e_real_file.cpp
    LIBRARIES exosnap_engine_testutil
)
target_compile_definitions(test_session_e2e_real_file PRIVATE NOMINMAX)

# Powerloss CI proxy: truncate a genuinely-flushed synthetic MKV at assorted
# offsets and assert the RemuxToMkv repair path never crashes and salvages up to
# the last complete cluster. Not `live` — deterministic and device-free.
exosnap_add_gtest(
    NAME recovery_truncation_tests
    TEST_PREFIX engine.
    SOURCES tests/test_recovery_truncation.cpp
    LIBRARIES exosnap_engine_testutil
)
target_compile_definitions(recovery_truncation_tests PRIVATE NOMINMAX)

# ADR 0035 (0.8.0): phase-correct CFR pacing ring-size helper (pure; no D3D/NVENC).
exosnap_add_gtest(
    NAME frame_pacing_tests
    TEST_PREFIX frame_pacing.
    SOURCES tests/test_frame_pacing.cpp
            src/frame_pacing.cpp
)
target_include_directories(frame_pacing_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Retained-frame plan: VisualFrameKey/VisualGenerations — pure value types, no D3D/NVENC.
exosnap_add_gtest(
    NAME test_visual_generations
    TEST_PREFIX engine.
    SOURCES tests/test_visual_generations.cpp
)
target_include_directories(test_visual_generations PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Retained-frame plan: separates DXGI cursor-only acquires from real desktop
# presents (CV-CURSOR-001) — pure logic, no D3D/NVENC.
exosnap_add_gtest(
    NAME test_od_acquire_classify
    TEST_PREFIX engine.
    SOURCES tests/test_od_acquire_classify.cpp
)
target_include_directories(test_od_acquire_classify PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# WGC acquire failure classes shared by the recording worker and the preview
# producer: pure HRESULT logic, no D3D/WinRT.
exosnap_add_gtest(
    NAME test_wgc_acquire_classify
    TEST_PREFIX engine.
    SOURCES tests/test_wgc_acquire_classify.cpp
)
target_include_directories(test_wgc_acquire_classify PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Capture-card live wiring (0.8.0): pure stage-health resolver (no D3D/NVENC/Qt).
exosnap_add_gtest(
    NAME pipeline_health_tests
    TEST_PREFIX pipeline_health.
    SOURCES tests/test_pipeline_health.cpp
            src/pipeline_health.cpp
)
target_include_directories(pipeline_health_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Perf measurement infrastructure: the pure fixed-bucket latency histogram behind
# the whole-session encode-latency / frame-time distributions (no GPU/NVENC/Qt).
exosnap_add_gtest(
    NAME test_perf_histogram
    TEST_PREFIX engine.
    SOURCES tests/test_perf_histogram.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_perf_histogram PRIVATE src)

# analyze-encode-perf.py round-trip: write a synthetic engine.jsonl via the real
# logging layer and assert the script recomputes the same percentiles from the
# summary histogram. Skips itself when no Python interpreter is present.
exosnap_add_gtest(
    NAME test_analyze_encode_perf
    TEST_PREFIX engine.
    SOURCES tests/test_analyze_encode_perf.cpp
    LIBRARIES exosnap_engine
)
target_include_directories(test_analyze_encode_perf PRIVATE src)
target_compile_definitions(test_analyze_encode_perf PRIVATE EXOSNAP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# The audio capture seam with no device behind it. Windows has no user-mode
# virtual audio device (that is a kernel driver), and this reaches states a real
# card will not produce on demand anyway: a measured gap, an unmeasured
# discontinuity, a drifting device clock, digital silence that is not a dead
# endpoint. The fake is itself tested -- an instrument that lies makes every
# result taken with it look like evidence.
exosnap_add_gtest(
    NAME test_fake_audio_capture_source
    TEST_PREFIX engine.
    SOURCES tests/test_fake_audio_capture_source.cpp
)
target_include_directories(test_fake_audio_capture_source
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests)
