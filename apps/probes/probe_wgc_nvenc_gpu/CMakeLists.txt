set(_nvenc_header "${CMAKE_SOURCE_DIR}/third_party/nvidia/nvEncodeAPI.h")

if(NOT EXISTS "${_nvenc_header}")
    message(STATUS "probe_wgc_nvenc_gpu: nvEncodeAPI.h not found at ${_nvenc_header} — skipping target")
    message(STATUS "  Place nvEncodeAPI.h in third_party/nvidia/ from the NVIDIA Video Codec SDK")
    return()
endif()

add_executable(probe_wgc_nvenc_gpu
    src/main.cpp
    src/wgc_nvenc_gpu_probe.h
    src/wgc_nvenc_gpu_probe.cpp
)

target_link_libraries(probe_wgc_nvenc_gpu PRIVATE
    d3d11.lib
    dxgi.lib
    dwmapi.lib
    user32.lib
    runtimeobject.lib
    exosnap::warnings
)

target_include_directories(probe_wgc_nvenc_gpu PRIVATE
    src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia"
)

set_property(TARGET probe_wgc_nvenc_gpu PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_wgc_nvenc_gpu PROPERTY CXX_STANDARD_REQUIRED ON)
