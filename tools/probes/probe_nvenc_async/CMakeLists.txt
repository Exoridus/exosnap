# probe_nvenc_async — live-verify probe for the async NVENC path. Drives
# the ACTUAL recorder_core
# NvencVideoEncoder/NvencEncoder classes through a real D3D11 + NVENC async
# session (not a reimplementation of raw SDK calls, unlike probe_nvenc), so
# the exact shipped code is what gets exercised. Never touches the ExoSnap
# application itself.
#
# Requires the full recorder_core build (NVENC headers present); skipped
# otherwise. Needs recorder_core's private src/ headers (NvencVideoEncoder is
# not part of the public include/recorder_core API), so this target adds that
# include path explicitly rather than relying on recorder_core's own (PRIVATE)
# one.

if(NOT TARGET recorder_core)
    message(STATUS "probe_nvenc_async: recorder_core target not available — skipping")
    return()
endif()

add_executable(probe_nvenc_async
    src/main.cpp
)

target_link_libraries(probe_nvenc_async PRIVATE
    recorder_core
    exosnap::warnings
    d3d11.lib
    dxgi.lib
)

target_include_directories(probe_nvenc_async PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/recorder_core/src"
    "${CMAKE_SOURCE_DIR}/third_party/nvidia"
)

target_compile_definitions(probe_nvenc_async PRIVATE NOMINMAX)

set_property(TARGET probe_nvenc_async PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_nvenc_async PROPERTY CXX_STANDARD_REQUIRED ON)
