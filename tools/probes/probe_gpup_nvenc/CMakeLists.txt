# probe_gpup_nvenc -- NVENC through a GPU partition.
#
# Same header dependency as probe_nvenc: without the Video Codec SDK header there is
# no target, rather than a configure failure in a tree that never asked for probes.

set(_gpup_nvenc_header "${CMAKE_SOURCE_DIR}/third_party/nvidia/nvEncodeAPI.h")

if(NOT EXISTS "${_gpup_nvenc_header}")
    message(STATUS "probe_gpup_nvenc: nvEncodeAPI.h not found at ${_gpup_nvenc_header} -- skipping target")
    message(STATUS "  Download the NVIDIA Video Codec SDK headers from:")
    message(STATUS "  https://developer.nvidia.com/video-codec-sdk")
    message(STATUS "  and place nvEncodeAPI.h in third_party/nvidia/")
    return()
endif()

add_executable(probe_gpup_nvenc
    src/main.cpp
)

target_link_libraries(probe_gpup_nvenc PRIVATE
    d3d11.lib
    dxgi.lib
    exosnap::warnings
)

target_include_directories(probe_gpup_nvenc PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/nvidia"
)

target_compile_definitions(probe_gpup_nvenc PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_property(TARGET probe_gpup_nvenc PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_gpup_nvenc PROPERTY CXX_STANDARD_REQUIRED ON)
