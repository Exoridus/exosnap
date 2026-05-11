set(_nvenc_header "${CMAKE_SOURCE_DIR}/third_party/nvidia/nvEncodeAPI.h")

if(NOT EXISTS "${_nvenc_header}")
    message(STATUS "probe_wgc_nvenc: nvEncodeAPI.h not found at ${_nvenc_header} — skipping target")
    message(STATUS "  Place nvEncodeAPI.h in third_party/nvidia/ from the NVIDIA Video Codec SDK")
    return()
endif()

add_executable(probe_wgc_nvenc
    src/main.cpp
    src/wgc_nvenc_probe.h
    src/wgc_nvenc_probe.cpp
)

target_link_libraries(probe_wgc_nvenc PRIVATE
    d3d11.lib
    dxgi.lib
    user32.lib
    runtimeobject.lib
    exosnap::warnings
)

target_include_directories(probe_wgc_nvenc PRIVATE
    src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia"
)
