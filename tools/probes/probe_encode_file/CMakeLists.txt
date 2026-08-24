# probe_encode_file — dev-only probe: reads a Y4M reference clip, encodes it
# through the real NvencVideoEncoder (the exact class the product uses), and
# writes a raw elementary stream. Backs scripts/dev/encoder_quality_matrix.py.
# Never packaged.
#
# Requires the full exosnap_engine build (NVENC headers present); skipped otherwise.

if(NOT TARGET exosnap_engine)
    message(STATUS "probe_encode_file: exosnap_engine target not available — skipping")
    return()
endif()

add_executable(probe_encode_file
    src/main.cpp
    # These Y4M/pixel-format/IVF-framing helpers are compiled only into their
    # own gtest targets (test_y4m_reader/test_yuv_convert/
    # test_elementary_stream_writer) — they are dev-tool-only and
    # intentionally not part of the exosnap_engine library sources, so this
    # probe compiles them directly.
    "${CMAKE_SOURCE_DIR}/libs/engine/src/y4m_reader.cpp"
    "${CMAKE_SOURCE_DIR}/libs/engine/src/yuv_convert.cpp"
    "${CMAKE_SOURCE_DIR}/libs/engine/src/elementary_stream_writer.cpp"
)

target_link_libraries(probe_encode_file PRIVATE
    exosnap_engine
    exosnap::warnings
    d3d11.lib
)

target_include_directories(probe_encode_file PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/engine/src"
    "${CMAKE_SOURCE_DIR}/third_party/nvidia"
)

target_compile_definitions(probe_encode_file PRIVATE NOMINMAX)

set_property(TARGET probe_encode_file PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_encode_file PROPERTY CXX_STANDARD_REQUIRED ON)
