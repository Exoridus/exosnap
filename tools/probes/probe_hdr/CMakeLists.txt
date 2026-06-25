# probe_hdr — HDR-readiness spike.
#
# Answers two questions for the HDR10 slice, on the real GPU/displays:
#   1. Does the D3D11 VideoProcessor support converting the captured HDR desktop
#      (scRGB FP16, R16G16B16A16_FLOAT) to PQ/BT.2020 P010 directly? If yes the
#      HDR conversion is "free" like the SDR path; if no it needs a compute shader.
#   2. Which displays are HDR-capable / currently in HDR mode, and what mastering
#      info (primaries, luminance) do they report (IDXGIOutput6::GetDesc1)?
#
# Pure D3D11/DXGI — no recorder_core dependency. Built only with -DEXOSNAP_BUILD_PROBES=ON.

add_executable(probe_hdr
    src/main.cpp
)

target_link_libraries(probe_hdr PRIVATE
    d3d11.lib
    dxgi.lib
    exosnap::warnings
)

target_compile_definitions(probe_hdr PRIVATE NOMINMAX)

set_property(TARGET probe_hdr PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_hdr PROPERTY CXX_STANDARD_REQUIRED ON)
