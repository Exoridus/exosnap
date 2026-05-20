set(_nvenc_header "${CMAKE_SOURCE_DIR}/third_party/nvidia/nvEncodeAPI.h")

if(NOT EXISTS "${_nvenc_header}")
    message(STATUS "recorder_core: nvEncodeAPI.h not found at ${_nvenc_header}")
    message(STATUS "  Place nvEncodeAPI.h in third_party/nvidia/ to enable the full recorder_core build")
    message(STATUS "  Falling back to skeleton build (version + logging only)")

    add_library(recorder_core STATIC
        src/version.cpp
        src/logging/logging.cpp
        src/audio_track_model.cpp
    )

    target_include_directories(recorder_core PUBLIC include)

    set_target_properties(recorder_core PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    target_link_libraries(recorder_core PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        exosnap::warnings
    )

    exosnap_add_gtest(
        NAME test_audio_track_resolver
        TEST_PREFIX recorder_core.
        SOURCES tests/test_audio_track_resolver.cpp
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
    src/codec_private.cpp
    src/wgc_capture.cpp
    src/nvenc_encoder.cpp
    src/mf_aac_encoder.cpp
    src/wasapi_loopback.cpp
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
