# probe_edit_playback — dev-only MEASUREMENT probe (no product code changed).
# Drives the REAL exosnap::engine::EditPlayerEngine class (the exact code the
# Edit-page player uses) through Open() + StartPlaybackDecode() at maximum
# throughput (callbacks just count-and-drop, no WASAPI/pacing) to measure
# whether the single decode thread can sustain real-time 1440p60 playback.
# Also isolates the YUV->BGRA conversion cost, the per-frame BGRA buffer
# allocation cost, and FFmpeg's default H.264 decoder threading. Read-only:
# never writes to its input file. Backs the 2026-08-01 edit-player-stutter
# investigation. Never touches the ExoSnap application itself.
#
# Requires the full exosnap_engine build (FFmpeg/NVENC headers present); skipped otherwise.

if(NOT TARGET exosnap_engine)
    message(STATUS "probe_edit_playback: exosnap_engine target not available — skipping")
    return()
endif()

add_executable(probe_edit_playback
    src/main.cpp
    src/step_g_autovec.cpp
    src/step_g_sse.cpp
    src/step_g_avx2.cpp
    src/step_g_cpuid.cpp
)

# Step G (SIMD-vs-scalar investigation, 2026-08-01): step_g_autovec.cpp is a
# 1:1 copy of the scalar loop compiled with a wider target ISA to test
# whether /arch:AVX2 alone lets the MSVC auto-vectorizer speed it up, and
# step_g_avx2.cpp hand-writes AVX2 intrinsics -- both need /arch:AVX2 for
# correct/intended codegen. step_g_sse.cpp (SSE2/SSE4.1) and step_g_cpuid.cpp
# (must run safely on pre-AVX2 CPUs) deliberately do NOT get this flag.
set_source_files_properties(
    src/step_g_autovec.cpp
    src/step_g_avx2.cpp
    PROPERTIES COMPILE_OPTIONS "/arch:AVX2"
)

target_link_libraries(probe_edit_playback PRIVATE
    exosnap_engine
    exosnap::warnings
    FFmpeg::mux
)

# yuv_to_bgra.h is a private exosnap_engine header (not part of the public
# include/recorder_core API) — needed here for step C's isolated conversion
# measurement. recorder_core.lib already contains the compiled
# ConvertFullPlanarYuv420ToBgra symbol (edit_player_engine.cpp calls it), so
# no extra source needs recompiling — just the include path for the
# declaration.
target_include_directories(probe_edit_playback PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/engine/src"
)

target_compile_definitions(probe_edit_playback PRIVATE NOMINMAX)

set_property(TARGET probe_edit_playback PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_edit_playback PROPERTY CXX_STANDARD_REQUIRED ON)

# Stage the FFmpeg runtime DLLs next to the probe exe. exosnap_engine links
# FFmpeg PRIVATE, so its DLLs are not otherwise placed anywhere near this
# target's own output directory (tools/probes/probe_edit_playback/<config>/) —
# without this the exe fails to start (missing avformat-*.dll etc.) unless
# FFmpeg's bin/ happens to already be on PATH. Mirrors the pattern in
# cmake/exosnap_testing.cmake (exosnap_add_gtest), scaled down to just FFmpeg
# since this probe has no Qt dependency.
foreach(_probe_ffmpeg_dll IN LISTS EXOSNAP_FFMPEG_DLLS)
    add_custom_command(TARGET probe_edit_playback POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_probe_ffmpeg_dll}" "$<TARGET_FILE_DIR:probe_edit_playback>")
endforeach()
