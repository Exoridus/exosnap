set(_nvenc_header "${CMAKE_SOURCE_DIR}/third_party/nvidia/nvEncodeAPI.h")

if(NOT EXISTS "${_nvenc_header}")
    message(STATUS "probe_nvenc: nvEncodeAPI.h not found at ${_nvenc_header} — skipping target")
    message(STATUS "  Download the NVIDIA Video Codec SDK headers from:")
    message(STATUS "  https://developer.nvidia.com/video-codec-sdk")
    message(STATUS "  and place nvEncodeAPI.h in third_party/nvidia/")
    return()
endif()

add_executable(probe_nvenc
    src/main.cpp
    src/nvenc_probe.h
    src/nvenc_probe.cpp
)

target_link_libraries(probe_nvenc PRIVATE
    d3d11.lib
    dxgi.lib
    exosnap::warnings
)

target_include_directories(probe_nvenc PRIVATE
    src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia"
)
