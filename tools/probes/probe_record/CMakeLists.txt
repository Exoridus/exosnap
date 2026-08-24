# probe_record — headless recording probe driving the real RecorderSession pipeline.
#
# Records the primary monitor for N seconds to a chosen container/codec, going
# through the exact production path (WGC capture -> composit -> NVENC -> mux ->
# MP4 remux-on-stop). Produces a real file for ffprobe/mediainfo/mkvinfo
# verification of codec, container tags (hvc1 vs hev1), colour, and bit depth.
#
# Requires the full exosnap_engine build (NVENC headers present); skipped otherwise.

if(NOT TARGET exosnap_engine)
    message(STATUS "probe_record: exosnap_engine target not available — skipping")
    return()
endif()

add_executable(probe_record
    src/main.cpp
)

target_link_libraries(probe_record PRIVATE
    exosnap_engine
    exosnap::warnings
    dxgi.lib
)

target_compile_definitions(probe_record PRIVATE NOMINMAX)

set_property(TARGET probe_record PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_record PROPERTY CXX_STANDARD_REQUIRED ON)
