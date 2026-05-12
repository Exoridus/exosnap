set(_nvenc_header "${CMAKE_SOURCE_DIR}/third_party/nvidia/nvEncodeAPI.h")

if(NOT EXISTS "${_nvenc_header}")
    message(STATUS "probe_wgc_nvenc_aac_mkv: nvEncodeAPI.h not found at ${_nvenc_header} — skipping target")
    message(STATUS "  Place nvEncodeAPI.h in third_party/nvidia/ from the NVIDIA Video Codec SDK")
    return()
endif()

add_executable(probe_wgc_nvenc_aac_mkv
    src/main.cpp
    src/wgc_nvenc_aac_mkv_probe.h
    src/wgc_nvenc_aac_mkv_probe.cpp
)

target_link_libraries(probe_wgc_nvenc_aac_mkv PRIVATE
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

# Resolve libwebm source directory via FetchContent properties
FetchContent_GetProperties(libwebm SOURCE_DIR _libwebm_src)

target_include_directories(probe_wgc_nvenc_aac_mkv PRIVATE
    src
    "${CMAKE_SOURCE_DIR}/third_party/nvidia"
    "${_libwebm_src}"
)

set_property(TARGET probe_wgc_nvenc_aac_mkv PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_wgc_nvenc_aac_mkv PROPERTY CXX_STANDARD_REQUIRED ON)
